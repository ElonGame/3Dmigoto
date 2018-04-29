#include "CommandList.h"

#include <DDSTextureLoader.h>
#include <WICTextureLoader.h>
#include <algorithm>
#include <sstream>
#include "HackerDevice.h"
#include "HackerContext.h"
#include "Override.h"
#include "D3D11Wrapper.h"
#include "IniHandler.h"
#include "profiling.h"

#include <D3DCompiler.h>

CustomResources customResources;
CustomShaders customShaders;
ExplicitCommandListSections explicitCommandListSections;
std::vector<CommandList*> registered_command_lists;
std::unordered_set<CommandList*> command_lists_profiling;
std::unordered_set<CommandListCommand*> command_lists_cmd_profiling;
std::vector<std::shared_ptr<CommandList>> dynamically_allocated_command_lists;


// Adds consistent "3DMigoto" prefix to frame analysis log with appropriate
// level of indentation for the current recursion level. Using a
// macro instead of a function for this to concatenate static strings:
#define COMMAND_LIST_LOG(state, fmt, ...) \
	do { \
		(state)->mHackerContext->FrameAnalysisLog("3DMigoto%*s " fmt, state->recursion, "", __VA_ARGS__); \
	} while (0)

struct command_list_profiling_state {
	LARGE_INTEGER list_start_time;
	LARGE_INTEGER cmd_start_time;
	LARGE_INTEGER saved_recursive_time;
};

static inline void profile_command_list_start(CommandList *command_list, CommandListState *state,
		command_list_profiling_state *profiling_state)
{
	bool inserted;

	if ((Profiling::mode != Profiling::Mode::SUMMARY)
	 && (Profiling::mode != Profiling::Mode::TOP_COMMAND_LISTS))
		return;

	inserted = command_lists_profiling.insert(command_list).second;
	if (inserted) {
		command_list->time_spent_inclusive.QuadPart = 0;
		command_list->time_spent_exclusive.QuadPart = 0;
		command_list->executions = 0;
	}

	profiling_state->saved_recursive_time = state->profiling_time_recursive;
	state->profiling_time_recursive.QuadPart = 0;

	QueryPerformanceCounter(&profiling_state->list_start_time);
}

static inline void profile_command_list_end(CommandList *command_list, CommandListState *state,
		command_list_profiling_state *profiling_state)
{
	LARGE_INTEGER list_end_time, duration;

	if ((Profiling::mode != Profiling::Mode::SUMMARY)
	 && (Profiling::mode != Profiling::Mode::TOP_COMMAND_LISTS))
		return;

	QueryPerformanceCounter(&list_end_time);
	duration.QuadPart = list_end_time.QuadPart - profiling_state->list_start_time.QuadPart;
	command_list->time_spent_inclusive.QuadPart += duration.QuadPart;
	command_list->time_spent_exclusive.QuadPart += duration.QuadPart - state->profiling_time_recursive.QuadPart;
	command_list->executions++;
	state->profiling_time_recursive.QuadPart = profiling_state->saved_recursive_time.QuadPart + duration.QuadPart;
}

static inline void profile_command_list_cmd_start(CommandListCommand *cmd,
		command_list_profiling_state *profiling_state)
{
	bool inserted;

	if (Profiling::mode != Profiling::Mode::TOP_COMMANDS)
		return;

	inserted = command_lists_cmd_profiling.insert(cmd).second;
	if (inserted) {
		cmd->pre_time_spent.QuadPart = 0;
		cmd->post_time_spent.QuadPart = 0;
		cmd->pre_executions = 0;
		cmd->post_executions = 0;
	}

	QueryPerformanceCounter(&profiling_state->cmd_start_time);
}

static inline void profile_command_list_cmd_end(CommandListCommand *cmd, CommandListState *state,
		command_list_profiling_state *profiling_state)
{
	LARGE_INTEGER end_time;

	if (Profiling::mode != Profiling::Mode::TOP_COMMANDS)
		return;

	QueryPerformanceCounter(&end_time);
	if (state->post) {
		cmd->post_time_spent.QuadPart += end_time.QuadPart - profiling_state->cmd_start_time.QuadPart;
		cmd->post_executions++;
	} else {
		cmd->pre_time_spent.QuadPart += end_time.QuadPart - profiling_state->cmd_start_time.QuadPart;
		cmd->pre_executions++;
	}
}

static void _RunCommandList(CommandList *command_list, CommandListState *state)
{
	CommandList::Commands::iterator i;
	command_list_profiling_state profiling_state;

	if (state->recursion > MAX_COMMAND_LIST_RECURSION) {
		LogInfo("WARNING: Command list recursion limit exceeded! Circular reference?\n");
		return;
	}

	if (command_list->commands.empty())
		return;

	COMMAND_LIST_LOG(state, "%s {\n", state->post ? "post" : "pre");
	state->recursion++;

	profile_command_list_start(command_list, state, &profiling_state);

	for (i = command_list->commands.begin(); i < command_list->commands.end() && !state->aborted; i++) {
		profile_command_list_cmd_start(i->get(), &profiling_state);
		(*i)->run(state);
		profile_command_list_cmd_end(i->get(), state, &profiling_state);
	}

	profile_command_list_end(command_list, state, &profiling_state);

	state->recursion--;
	COMMAND_LIST_LOG(state, "}\n");
}

static void CommandListFlushState(CommandListState *state)
{
	D3D11_MAPPED_SUBRESOURCE mappedResource;
	HRESULT hr;

	if (state->update_params) {
		hr = state->mOrigContext1->Map(state->mHackerDevice->mIniTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		if (FAILED(hr)) {
			LogInfo("CommandListFlushState: Map failed\n");
			return;
		}
		memcpy(mappedResource.pData, &G->iniParams, sizeof(G->iniParams));
		state->mOrigContext1->Unmap(state->mHackerDevice->mIniTexture, 0);
		state->update_params = false;
	}
}

static void RunCommandListComplete(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list,
		DrawCallInfo *call_info,
		ID3D11Resource *resource,
		ID3D11View *view,
		bool post)
{
	CommandListState state;
	state.mHackerDevice = mHackerDevice;
	state.mHackerContext = mHackerContext;
	state.mOrigDevice1 = mHackerDevice->GetPassThroughOrigDevice1();
	state.mOrigContext1 = mHackerContext->GetPassThroughOrigContext1();

	state.call_info = call_info;
	state.resource = resource;
	state.view = view;
	state.post = post;

	_RunCommandList(command_list, &state);
	CommandListFlushState(&state);
}

void RunCommandList(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list,
		DrawCallInfo *call_info,
		bool post)
{
	ID3D11Resource *resource = NULL;
	if (call_info)
		resource = call_info->indirect_buffer;

	RunCommandListComplete(mHackerDevice, mHackerContext, command_list,
			call_info, resource, NULL, post);
}

void RunResourceCommandList(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list,
		ID3D11Resource *resource,
		bool post)
{
	RunCommandListComplete(mHackerDevice, mHackerContext, command_list,
			NULL, resource, NULL, post);
}

void RunViewCommandList(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list,
		ID3D11View *view,
		bool post)
{
	ID3D11Resource *res = NULL;

	if (view)
		view->GetResource(&res);

	RunCommandListComplete(mHackerDevice, mHackerContext, command_list,
			NULL, res, view, post);

	if (res)
		res->Release();
}

void optimise_command_lists(HackerDevice *device)
{
	bool making_progress;
	bool ignore_cto, ignore_cto_pre, ignore_cto_post;
	int i;
	CommandList::Commands::iterator new_end;

	do {
		making_progress = false;
		ignore_cto_pre = true;
		ignore_cto_post = true;

		// If all TextureOverride sections have empty command lists of
		// either pre or post, we can treat checktextureoverride as a
		// noop. This is intended to catch the case where we only have
		// "pre" commands in the TextureOverride sections to optimise
		// out the implicit "post checktextureoverride" commands, as
		// these can add up if they are used on very common shaders
		// (e.g. in DOAXVV this can easily save 0.2fps on a 4GHz CPU,
		// more on a slower CPU since this command is used in the
		// shadow map shaders)
		//
		// FIXME: This should itself ignore any checktextureoverrides
		// inside these command lists
		for (auto &tolkv : G->mTextureOverrideMap) {
			for (TextureOverride &to : tolkv.second) {
				ignore_cto_pre = ignore_cto_pre && to.command_list.commands.empty();
				ignore_cto_post = ignore_cto_post && to.post_command_list.commands.empty();
			}
		}
		for (auto &tof : G->mFuzzyTextureOverrides) {
			ignore_cto_pre = ignore_cto_pre && tof->texture_override->command_list.commands.empty();
			ignore_cto_post = ignore_cto_post && tof->texture_override->post_command_list.commands.empty();
		}

		// Go through each registered command list and remove any
		// commands that are noops to eliminate the runtime overhead of
		// processing these
		for (CommandList *command_list : registered_command_lists) {
			ignore_cto = ignore_cto_pre;
			if (command_list->post)
				ignore_cto = ignore_cto_post;

			for (i = 0; i < command_list->commands.size(); ) {
				LogInfo("Optimising %S\n", command_list->commands[i]->ini_line.c_str());
				if (command_list->commands[i]->optimise(device))
					making_progress = true;

				if (command_list->commands[i]->noop(command_list->post, ignore_cto)) {
					LogInfo("Optimised out %s %S\n",
							command_list->post ? "post" : "pre",
							command_list->commands[i]->ini_line.c_str());
					command_list->commands.erase(command_list->commands.begin() + i);
					making_progress = true;
					continue;
				}
				i++;
			}
		}

		// TODO: Merge adjacent commands if possible, e.g. all the
		// commands in BuiltInCommandListUnbindAllRenderTargets would
		// be good candidates to merge into a single command. We could
		// add a special command for that particular case, but would be
		// nice if this sort of thing worked more generally.
	} while (making_progress);

	registered_command_lists.clear();
	dynamically_allocated_command_lists.clear();
}

static bool AddCommandToList(CommandListCommand *command,
		CommandList *explicit_command_list,
		CommandList *sensible_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wchar_t *section,
		const wchar_t *key, wstring *val)
{
	if (section && key) {
		command->ini_line = L"[" + wstring(section) + L"] " + wstring(key);
		if (val)
			command->ini_line += L" = " + *val;
	}

	if (explicit_command_list) {
		// User explicitly specified "pre" or "post", so only add the
		// command to that list
		explicit_command_list->commands.push_back(std::shared_ptr<CommandListCommand>(command));
	} else if (sensible_command_list) {
		// User did not specify which command list to add it to, but
		// the command they specified has a sensible default, so add it
		// to that list:
		sensible_command_list->commands.push_back(std::shared_ptr<CommandListCommand>(command));
	} else {
		// The command's default is to add it to both lists (e.g. the
		// checktextureoverride directive will call command lists in
		// another ini section with both pre and post lists, so the
		// principal of least unexpected behaviour says we add it to
		// both so that both those command lists will be called)
		//
		// Using a std::shared_ptr here to handle adding the same
		// pointer to two lists and have it garbage collected only once
		// both are destroyed:
		std::shared_ptr<CommandListCommand> p(command);
		pre_command_list->commands.push_back(p);
		if (post_command_list)
			post_command_list->commands.push_back(p);
	}

	return true;
}

static bool ParseCheckTextureOverride(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	int ret;

	CheckTextureOverrideCommand *operation = new CheckTextureOverrideCommand();

	// Parse value as consistent with texture filtering and resource copying
	ret = operation->target.ParseTarget(val->c_str(), true, ini_namespace);
	if (ret) {
		return AddCommandToList(operation, explicit_command_list, NULL, pre_command_list, post_command_list, section, key, val);
	}

	delete operation;
	return false;
}

static bool ParseResetPerFrameLimits(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	CustomResources::iterator res;
	CustomShaders::iterator shader;
	wstring namespaced_section;

	ResetPerFrameLimitsCommand *operation = new ResetPerFrameLimitsCommand();

	if (!wcsncmp(val->c_str(), L"resource", 8)) {
		wstring resource_id(val->c_str());

		res = customResources.end();
		if (get_namespaced_section_name_lower(&resource_id, ini_namespace, &namespaced_section))
			res = customResources.find(namespaced_section);
		if (res == customResources.end())
			res = customResources.find(resource_id);
		if (res == customResources.end())
			goto bail;

		operation->resource = &res->second;
	}

	if (!wcsncmp(val->c_str(), L"customshader", 12) || !wcsncmp(val->c_str(), L"builtincustomshader", 19)) {
		wstring shader_id(val->c_str());

		shader = customShaders.end();
		if (get_namespaced_section_name_lower(&shader_id, ini_namespace, &namespaced_section))
			shader = customShaders.find(namespaced_section);
		if (shader == customShaders.end())
			shader = customShaders.find(shader_id);
		if (shader == customShaders.end())
			goto bail;

		operation->shader = &shader->second;
	}

	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParseClearView(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	CustomResources::iterator res;
	CustomShaders::iterator shader;
	wistringstream token_stream(*val);
	wstring token;
	int ret, len1;
	int idx = 0;
	unsigned uval;
	float fval;

	ClearViewCommand *operation = new ClearViewCommand();

	while (getline(token_stream, token, L' ')) {
		if (operation->target.type == ResourceCopyTargetType::INVALID) {
			ret = operation->target.ParseTarget(token.c_str(), true, ini_namespace);
			if (ret)
				continue;
		}

		if (idx < 4) {
			// Try parsing value as a hex string. If this matches
			// we know the user didn't intend to use floats. This
			// is necessary to allow integer values that require
			// more than 24 significant bits to be used, which
			// would be lost if we only parsed the string as a
			// float, e.g. 0xffffffff cannot be stored as a float
			ret = swscanf_s(token.c_str(), L"0x%x%n", &uval, &len1);
			if (ret != 0 && ret != EOF && len1 == token.length()) {
				operation->uval[idx] = uval;
				operation->fval[idx] = *(float*)&uval;
				operation->clear_uav_uint = true;
				idx++;
				continue;
			}

			// On the other hand, if parsing the value as a float
			// matches the user might have intended it to be a
			// float or an integer. We will assume they want floats
			// by default, but store it in both arrays in case we
			// later determine that we need to use an integer clear.
			ret = swscanf_s(token.c_str(), L"%f%n", &fval, &len1);
			if (ret != 0 && ret != EOF && len1 == token.length()) {
				operation->fval[idx] = fval;
				operation->uval[idx] = (UINT)fval;
				idx++;
				continue;
			}
		}
		if (!wcscmp(token.c_str(), L"int")) {
			operation->clear_uav_uint = true;
			continue;
		}
		if (!wcscmp(token.c_str(), L"depth")) {
			operation->clear_depth = true;
			continue;
		}
		if (!wcscmp(token.c_str(), L"stencil")) {
			operation->clear_stencil = true;
			continue;
		}

		goto bail;
	}

	if (operation->target.type == ResourceCopyTargetType::INVALID)
		goto bail;

	// Use the first value specified as the depth value when clearing a
	// DSV, and the second as the stencil value, unless we are only
	// clearing the stencil side, in which case use the first:
	operation->dsv_depth = operation->fval[0];
	operation->dsv_stencil = operation->uval[1];
	if (operation->clear_stencil && !operation->clear_depth)
		operation->dsv_stencil = operation->uval[0];

	// Propagate the final specified value to the remaining channels. This
	// allows a single value to be specified to clear all channels in RTVs
	// and UAVs. Note that this is done after noting the DSV values because
	// we never want to propagate the depth value to the stencil value:
	for (idx++; idx < 4; idx++) {
		operation->uval[idx] = operation->uval[idx - 1];
		operation->fval[idx] = operation->fval[idx - 1];
	}

	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}


static bool ParseRunShader(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	RunCustomShaderCommand *operation = new RunCustomShaderCommand();
	CustomShaders::iterator shader;
	wstring namespaced_section;

	// Value should already have been transformed to lower case from
	// ParseCommandList, so our keys will be consistent in the
	// unordered_map:
	wstring shader_id(val->c_str());

	shader = customShaders.end();
	if (get_namespaced_section_name_lower(&shader_id, ini_namespace, &namespaced_section))
		shader = customShaders.find(namespaced_section);
	if (shader == customShaders.end())
		shader = customShaders.find(shader_id);
	if (shader == customShaders.end())
		goto bail;

	operation->custom_shader = &shader->second;
	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}

bool ParseRunExplicitCommandList(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	RunExplicitCommandList *operation = new RunExplicitCommandList();
	ExplicitCommandListSections::iterator shader;
	wstring namespaced_section;

	// We need value in lower case so our keys will be consistent in the
	// unordered_map. ParseCommandList will have already done this, but the
	// Key/Preset parsing code will not have, and rather than require it to
	// we do it here:
	wstring section_id(val->c_str());
	std::transform(section_id.begin(), section_id.end(), section_id.begin(), ::towlower);

	shader = explicitCommandListSections.end();
	if (get_namespaced_section_name_lower(&section_id, ini_namespace, &namespaced_section))
		shader = explicitCommandListSections.find(namespaced_section);
	if (shader == explicitCommandListSections.end())
		shader = explicitCommandListSections.find(section_id);
	if (shader == explicitCommandListSections.end())
		goto bail;

	operation->command_list_section = &shader->second;
	// This function is nearly identical to ParseRunShader, but in case we
	// later refactor these together note that here we do not specify a
	// sensible command list, so it will be added to both pre and post
	// command lists:
	return AddCommandToList(operation, explicit_command_list, NULL, pre_command_list, post_command_list, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParsePreset(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		bool exclude, const wstring *ini_namespace)
{
	PresetCommand *operation = new PresetCommand();
	wstring prefixed_section, namespaced_section;

	PresetOverrideMap::iterator i;

	// Value should already have been transformed to lower case from
	// ParseCommandList, so our keys will be consistent in the
	// unordered_map:
	wstring preset_id(val->c_str());

	// The original preset code did not accept the "Preset" prefix on the
	// prefix command, as in it would only accept 'preset = Foo', not
	// 'preset = PresetFoo'. While I agree that the later is redundant
	// since the word preset now appears twice, it is more consistent with
	// the way we have referenced other sections in the command list (ps-t0
	// = ResourceBar, run = CustomShaderBaz, etc), and it makes it easier
	// to search for 'PresetFoo' to find both where it is used and where it
	// is referenced, so it is good to support here... but for backwards
	// compatibility and less redundancy for those that prefer not to say
	// "preset" twice we support both ways.
	prefixed_section = wstring(L"preset") + preset_id;

	// And now with namespacing, we have four permutations to try...
	i = presetOverrides.end();
	// First, add the 'Preset' (i.e. the user did not) and try namespaced:
	if (get_namespaced_section_name_lower(&prefixed_section, ini_namespace, &namespaced_section))
		i = presetOverrides.find(namespaced_section);
	// Second, try namespaced without adding the prefix:
	if (i == presetOverrides.end()) {
		if (get_namespaced_section_name_lower(&preset_id, ini_namespace, &namespaced_section))
			i = presetOverrides.find(namespaced_section);
	}
	// Third, add the 'Preset' and try global:
	if (i == presetOverrides.end())
		i = presetOverrides.find(prefixed_section);
	// Finally, don't add the prefix and try global:
	if (i == presetOverrides.end())
		i = presetOverrides.find(preset_id);
	if (i == presetOverrides.end())
		goto bail;

	operation->preset = &i->second;
	operation->exclude = exclude;

	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParseDrawCommand(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list)
{
	DrawCommand *operation = new DrawCommand();
	int nargs, end = 0;

	if (!wcscmp(key, L"draw")) {
		if (!wcscmp(val->c_str(), L"from_caller")) {
			operation->type = DrawCommandType::FROM_CALLER;
			end = (int)val->length();
		} else {
			operation->type = DrawCommandType::DRAW;
			nargs = swscanf_s(val->c_str(), L"%u, %u%n", &operation->args[0], &operation->args[1], &end);
			if (nargs != 2)
				goto bail;
		}
	} else if (!wcscmp(key, L"drawauto")) {
		operation->type = DrawCommandType::DRAW_AUTO;
	} else if (!wcscmp(key, L"drawindexed")) {
		if (!wcscmp(val->c_str(), L"auto")) {
			operation->type = DrawCommandType::AUTO_INDEX_COUNT;
			end = (int)val->length();
		} else {
			operation->type = DrawCommandType::DRAW_INDEXED;
			nargs = swscanf_s(val->c_str(), L"%u, %u, %i%n", &operation->args[0], &operation->args[1], (INT*)&operation->args[2], &end);
			if (nargs != 3)
				goto bail;
		}
	} else if (!wcscmp(key, L"drawindexedinstanced")) {
		operation->type = DrawCommandType::DRAW_INDEXED_INSTANCED;
		nargs = swscanf_s(val->c_str(), L"%u, %u, %u, %i, %u%n",
				&operation->args[0], &operation->args[1], &operation->args[2], (INT*)&operation->args[3], &operation->args[4], &end);
		if (nargs != 5)
			goto bail;
	} else if (!wcscmp(key, L"drawinstanced")) {
		operation->type = DrawCommandType::DRAW_INSTANCED;
		nargs = swscanf_s(val->c_str(), L"%u, %u, %u, %u%n",
				&operation->args[0], &operation->args[1], &operation->args[2], &operation->args[3], &end);
		if (nargs != 5)
			goto bail;
	} else if (!wcscmp(key, L"dispatch")) {
		operation->type = DrawCommandType::DISPATCH;
		nargs = swscanf_s(val->c_str(), L"%u, %u, %u%n", &operation->args[0], &operation->args[1], &operation->args[2], &end);
		if (nargs != 3)
			goto bail;
	}

	// TODO: } else if (!wcscmp(key, L"drawindexedinstancedindirect")) {
	// TODO: 	operation->type = DrawCommandType::DRAW_INDEXED_INSTANCED_INDIRECT;
	// TODO: } else if (!wcscmp(key, L"drawinstancedindirect")) {
	// TODO: 	operation->type = DrawCommandType::DRAW_INSTANCED_INDIRECT;
	// TODO: } else if (!wcscmp(key, L"dispatchindirect")) {
	// TODO: 	operation->type = DrawCommandType::DISPATCH_INDIRECT;
	// TODO: }


	if (operation->type == DrawCommandType::INVALID)
		goto bail;

	if (end != val->length())
		goto bail;

	operation->ini_section = section;
	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParseDirectModeSetActiveEyeCommand(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list)
{
	DirectModeSetActiveEyeCommand *operation = new DirectModeSetActiveEyeCommand();

	if (!wcscmp(val->c_str(), L"mono")) {
		operation->eye = NVAPI_STEREO_EYE_MONO;
		goto success;
	}

	if (!wcscmp(val->c_str(), L"left")) {
		operation->eye = NVAPI_STEREO_EYE_LEFT;
		goto success;
	}

	if (!wcscmp(val->c_str(), L"right")) {
		operation->eye = NVAPI_STEREO_EYE_RIGHT;
		goto success;
	}

	goto bail;

success:
	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParsePerDrawStereoOverride(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		bool is_separation,
		const wstring *ini_namespace)
{
	bool restore_on_post = !explicit_command_list && pre_command_list && post_command_list;
	PerDrawStereoOverrideCommand *operation = NULL;

	if (is_separation)
		operation = new PerDrawSeparationOverrideCommand(restore_on_post);
	else
		operation = new PerDrawConvergenceOverrideCommand(restore_on_post);

	// Try parsing value as a resource target for staging auto-convergence
	// Do this first, because the operand parsing would treat these as for
	// texture filtering
	if (operation->staging_op.src.ParseTarget(val->c_str(), true, ini_namespace)) {
		operation->staging_type = true;
		goto success;
	}

	if (!operation->expression.parse(val, ini_namespace))
		goto bail;

success:
	// Add to both command lists by default - the pre command list will set
	// the value, and the post command list will restore the original. If
	// an explicit command list is specified then the value will only be
	// set, not restored (regardless of whether that is pre or post)
	return AddCommandToList(operation, explicit_command_list, NULL, pre_command_list, post_command_list, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParseFrameAnalysisDump(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	FrameAnalysisDumpCommand *operation = new FrameAnalysisDumpCommand();
	wchar_t *buf;
	size_t size = val->size() + 1;
	wchar_t *target = NULL;

	// parse_enum_option_string replaces spaces with NULLs, so it can't
	// operate on the buffer in the wstring directly. I could potentially
	// change it to work without modifying the string, but for now it's
	// easier to just make a copy of the string:
	buf = new wchar_t[size];
	wcscpy_s(buf, size, val->c_str());

	operation->analyse_options = parse_enum_option_string<wchar_t *, FrameAnalysisOptions>
		(FrameAnalysisOptionNames, buf, &target);

	if (!target)
		goto bail;

	if (!operation->target.ParseTarget(target, true, ini_namespace))
		goto bail;

	operation->target_name = L"[" + wstring(section) + L"]-" + wstring(target);
	// target_name will be used in the filenames, so replace any reserved characters:
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'<', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'>', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L':', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'"', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'/', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'\\',L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'|', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'?', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'*', L'_');

	delete [] buf;
	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete [] buf;
	delete operation;
	return false;
}

bool ParseCommandListGeneralCommands(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace)
{
	if (!wcscmp(key, L"checktextureoverride"))
		return ParseCheckTextureOverride(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);

	if (!wcscmp(key, L"run")) {
		if (!wcsncmp(val->c_str(), L"customshader", 12) || !wcsncmp(val->c_str(), L"builtincustomshader", 19))
			return ParseRunShader(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);

		if (!wcsncmp(val->c_str(), L"commandlist", 11) || !wcsncmp(val->c_str(), L"builtincommandlist", 18))
			return ParseRunExplicitCommandList(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);
	}

	if (!wcscmp(key, L"preset"))
		return ParsePreset(section, key, val, explicit_command_list, pre_command_list, post_command_list, false, ini_namespace);
	if (!wcscmp(key, L"exclude_preset"))
		return ParsePreset(section, key, val, explicit_command_list, pre_command_list, post_command_list, true, ini_namespace);

	if (!wcscmp(key, L"handling")) {
		// skip only makes sense in pre command lists, since it needs
		// to run before the original draw call:
		if (!wcscmp(val->c_str(), L"skip"))
			return AddCommandToList(new SkipCommand(section), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

		// abort defaults to both command lists, to abort command list
		// execution both before and after the draw call:
		if (!wcscmp(val->c_str(), L"abort"))
			return AddCommandToList(new AbortCommand(section), explicit_command_list, NULL, pre_command_list, post_command_list, section, key, val);
	}

	if (!wcscmp(key, L"reset_per_frame_limits"))
		return ParseResetPerFrameLimits(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);

	if (!wcscmp(key, L"clear"))
		return ParseClearView(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);

	if (!wcscmp(key, L"separation"))
		return ParsePerDrawStereoOverride(section, key, val, explicit_command_list, pre_command_list, post_command_list, true, ini_namespace);

	if (!wcscmp(key, L"convergence"))
		return ParsePerDrawStereoOverride(section, key, val, explicit_command_list, pre_command_list, post_command_list, false, ini_namespace);

	if (!wcscmp(key, L"direct_mode_eye"))
		return ParseDirectModeSetActiveEyeCommand(section, key, val, explicit_command_list, pre_command_list, post_command_list);

	if (!wcscmp(key, L"analyse_options"))
		return AddCommandToList(new FrameAnalysisChangeOptionsCommand(val), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

	if (!wcscmp(key, L"dump"))
		return ParseFrameAnalysisDump(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);

	if (!wcscmp(key, L"special")) {
		if (!wcscmp(val->c_str(), L"upscaling_switch_bb"))
			return AddCommandToList(new UpscalingFlipBBCommand(section), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);
	}

	return ParseDrawCommand(section, key, val, explicit_command_list, pre_command_list, post_command_list);
}

void CheckTextureOverrideCommand::run(CommandListState *state)
{
	TextureOverrideMatches matches;
	unsigned i;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	target.FindTextureOverrides(state, NULL, &matches);

	for (i = 0; i < matches.size(); i++) {
		if (state->post)
			_RunCommandList(&matches[i]->post_command_list, state);
		else
			_RunCommandList(&matches[i]->command_list, state);
	}
}

bool CheckTextureOverrideCommand::noop(bool post, bool ignore_cto)
{
	return ignore_cto;
}

ClearViewCommand::ClearViewCommand() :
	dsv_depth(0.0),
	dsv_stencil(0),
	clear_depth(false),
	clear_stencil(false),
	clear_uav_uint(false)
{
	memset(fval, 0, sizeof(fval));
	memset(uval, 0, sizeof(uval));
}

static bool UAVSupportsFloatClear(ID3D11UnorderedAccessView *uav)
{
	D3D11_UNORDERED_ACCESS_VIEW_DESC desc;

	// UAVs can be cleared as a float if their format is a float, snorm or
	// unorm, otherwise the clear will fail silently. I didn't include
	// partially typed or block compressed formats in the below list
	// because I doubt they would work (but haven't checked).

	uav->GetDesc(&desc);

	switch (desc.Format) {
		case DXGI_FORMAT_UNKNOWN:
			// Common case
			return false;
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R11G11B10_FLOAT:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_A8_UNORM:
		case DXGI_FORMAT_R1_UNORM:
		case DXGI_FORMAT_R8G8_B8G8_UNORM:
		case DXGI_FORMAT_G8R8_G8B8_UNORM:
		case DXGI_FORMAT_B5G6R5_UNORM:
		case DXGI_FORMAT_B5G5R5A1_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_B4G4R4A4_UNORM:
			return true;
		default:
			return false;
	}
}

void ResetPerFrameLimitsCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (shader)
		shader->executions_this_frame = 0;

	if (resource)
		resource->copies_this_frame = 0;
}

void PresetCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (exclude)
		preset->Exclude();
	else
		preset->Trigger(this);
}

static UINT get_index_count_from_current_ib(ID3D11DeviceContext *mOrigContext1)
{
	ID3D11Buffer *ib;
	D3D11_BUFFER_DESC desc;
	DXGI_FORMAT format;
	UINT offset;

	mOrigContext1->IAGetIndexBuffer(&ib, &format, &offset);
	if (!ib)
		return 0;

	ib->GetDesc(&desc);
	ib->Release();

	switch(format) {
		case DXGI_FORMAT_R16_UINT:
			return (desc.ByteWidth - offset) / 2;
		case DXGI_FORMAT_R32_UINT:
			return (desc.ByteWidth - offset) / 4;
	}

	return 0;
}

void DrawCommand::run(CommandListState *state)
{
	HackerContext *mHackerContext = state->mHackerContext;
	ID3D11DeviceContext *mOrigContext1 = state->mOrigContext1;
	DrawCallInfo *info = state->call_info;
	UINT auto_count = 0;

	// If this command list was triggered from something currently skipped
	// due to hunting, we also skip any custom draw calls, so that if we
	// are replacing the original draw call we will still be able to see
	// the object being hunted.
	if (info && info->hunting_skip) {
		COMMAND_LIST_LOG(state, "[%S] Draw -> SKIPPED DUE TO HUNTING\n", ini_section.c_str());
		return;
	}

	// Ensure IniParams are visible:
	CommandListFlushState(state);

	switch (type) {
		case DrawCommandType::DRAW:
			COMMAND_LIST_LOG(state, "[%S] Draw(%u, %u)\n", ini_section.c_str(), args[0], args[1]);
			mOrigContext1->Draw(args[0], args[1]);
			break;
		case DrawCommandType::DRAW_AUTO:
			COMMAND_LIST_LOG(state, "[%S] DrawAuto()\n", ini_section.c_str());
			mOrigContext1->DrawAuto();
			break;
		case DrawCommandType::DRAW_INDEXED:
			COMMAND_LIST_LOG(state, "[%S] DrawIndexed(%u, %u, %i)\n", ini_section.c_str(), args[0], args[1], (INT)args[2]);
			mOrigContext1->DrawIndexed(args[0], args[1], (INT)args[2]);
			break;
		case DrawCommandType::DRAW_INDEXED_INSTANCED:
			COMMAND_LIST_LOG(state, "[%S] DrawIndexedInstanced(%u, %u, %u, %i, %u)\n", ini_section.c_str(), args[0], args[1], args[2], (INT)args[3], args[4]);
			mOrigContext1->DrawIndexedInstanced(args[0], args[1], args[2], (INT)args[3], args[4]);
			break;
		// TODO: case DrawCommandType::DRAW_INDEXED_INSTANCED_INDIRECT:
		// TODO: 	break;
		case DrawCommandType::DRAW_INSTANCED:
			COMMAND_LIST_LOG(state, "[%S] DrawInstanced(%u, %u, %u, %u)\n", ini_section.c_str(), args[0], args[1], args[2], args[3]);
			mOrigContext1->DrawInstanced(args[0], args[1], args[2], args[3]);
			break;
		// TODO: case DrawCommandType::DRAW_INSTANCED_INDIRECT:
		// TODO: 	break;
		case DrawCommandType::DISPATCH:
			COMMAND_LIST_LOG(state, "[%S] Dispatch(%u, %u, %u)\n", ini_section.c_str(), args[0], args[1], args[2]);
			mOrigContext1->Dispatch(args[0], args[1], args[2]);
			break;
		// TODO: case DrawCommandType::DISPATCH_INDIRECT:
		// TODO: 	break;
		case DrawCommandType::FROM_CALLER:
			if (!info) {
				COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> NO ACTIVE DRAW CALL\n", ini_section.c_str());
				break;
			}
			switch (info->type) {
				case DrawCall::DrawIndexedInstanced:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawIndexedInstanced(%u, %u, %u, %i, %u)\n", ini_section.c_str(), info->IndexCount, info->InstanceCount, info->FirstIndex, info->FirstVertex, info->FirstInstance);
					mOrigContext1->DrawIndexedInstanced(info->IndexCount, info->InstanceCount, info->FirstIndex, info->FirstVertex, info->FirstInstance);
					break;
				case DrawCall::DrawInstanced:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawInstanced(%u, %u, %u, %u)\n", ini_section.c_str(), info->VertexCount, info->InstanceCount, info->FirstVertex, info->FirstInstance);
					mOrigContext1->DrawInstanced(info->VertexCount, info->InstanceCount, info->FirstVertex, info->FirstInstance);
					break;
				case DrawCall::DrawIndexed:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawIndexed(%u, %u, %i)\n", ini_section.c_str(), info->IndexCount, info->FirstIndex, info->FirstVertex);
					mOrigContext1->DrawIndexed(info->IndexCount, info->FirstIndex, info->FirstVertex);
					break;
				case DrawCall::Draw:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> Draw(%u, %u)\n", ini_section.c_str(), info->VertexCount, info->FirstVertex);
					mOrigContext1->Draw(info->VertexCount, info->FirstVertex);
					break;
				case DrawCall::DrawInstancedIndirect:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawInstancedIndirect(0x%p, %u)\n", ini_section.c_str(), info->indirect_buffer, info->args_offset);
					mOrigContext1->DrawInstancedIndirect(info->indirect_buffer, info->args_offset);
					break;
				case DrawCall::DrawIndexedInstancedIndirect:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawIndexedInstancedIndirect(0x%p, %u)\n", ini_section.c_str(), info->indirect_buffer, info->args_offset);
					mOrigContext1->DrawIndexedInstancedIndirect(info->indirect_buffer, info->args_offset);
					break;
				case DrawCall::DrawAuto:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawAuto()\n", ini_section.c_str());
					mOrigContext1->DrawAuto();
					break;
				default:
					LogInfo("BUG: draw = from_caller -> unknown draw call type\n");
					DoubleBeepExit();
			}
			// TODO: dispatch = from_caller
			break;
		case DrawCommandType::AUTO_INDEX_COUNT:
			auto_count = get_index_count_from_current_ib(mOrigContext1);
			COMMAND_LIST_LOG(state, "[%S] drawindexed = auto -> DrawIndexed(%u, 0, 0)\n", ini_section.c_str(), auto_count);
			if (auto_count)
				mOrigContext1->DrawIndexed(auto_count, 0, 0);
			else
				COMMAND_LIST_LOG(state, "  Unable to determine index count\n");
			break;
	}
}

void SkipCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "[%S] handling = skip\n", ini_section.c_str());

	if (state->call_info)
		state->call_info->skip = true;
	else
		COMMAND_LIST_LOG(state, "  No active draw call to skip\n");
}

void AbortCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "[%S] handling = abort\n", ini_section.c_str());

	state->aborted = true;
}

PerDrawStereoOverrideCommand::PerDrawStereoOverrideCommand(bool restore_on_post) :
		staging_type(false),
		val(FLT_MAX),
		saved(FLT_MAX),
		restore_on_post(restore_on_post),
		did_set_value_on_pre(false)
{}

bool PerDrawStereoOverrideCommand::update_val(CommandListState *state)
{
	D3D11_MAPPED_SUBRESOURCE mapping;
	HRESULT hr;
	float tmp;
	bool ret = false;

	if (!staging_type)
		return true;

	if (staging_op.staging) {
		hr = staging_op.map(state, &mapping);
		if (FAILED(hr)) {
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
				COMMAND_LIST_LOG(state, "  Transfer in progress...\n");
			else
				COMMAND_LIST_LOG(state, "  Unknown error: 0x%x\n", hr);
			return false;
		}

		// FIXME: Check if resource is at least 4 bytes (maybe we can
		// use RowPitch, but MSDN contradicts itself so I'm not sure.
		// Otherwise we can refer to the resource description)
		tmp = ((float*)mapping.pData)[0];
		staging_op.unmap(state);

		if (isnan(tmp)) {
			COMMAND_LIST_LOG(state, "  Disregarding NAN\n");
		} else {
			val = tmp;
			ret = true;
		}

		// To make auto-convergence as responsive as possible, we start
		// the next transfer as soon as we have retrieved the value
		// from the previous transfer. This should minimise the number
		// of frames displayed with wrong convergence on scene changes.
	}

	staging_op.staging = true;
	staging_op.run(state);
	return ret;
}

void PerDrawStereoOverrideCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (!state->mHackerDevice->mStereoHandle) {
		COMMAND_LIST_LOG(state, "  No Stereo Handle\n");
		return;
	}

	if (restore_on_post) {
		if (state->post) {
			if (!did_set_value_on_pre)
				return;
			did_set_value_on_pre = false;

			COMMAND_LIST_LOG(state, "  Restoring %s = %f\n", stereo_param_name(), saved);
			set_stereo_value(state, saved);
		} else {
			if (staging_type) {
				if (!(did_set_value_on_pre = update_val(state)))
					return;
			} else
				val = expression.evaluate(state);

			saved = get_stereo_value(state);

			COMMAND_LIST_LOG(state, "  Setting per-draw call %s = %f * %f = %f\n",
					stereo_param_name(), val, saved, val * saved);

			// The original ShaderOverride code multiplied the new
			// separation and convergence by the old ones, so I'm
			// doing that as well, but while that makes sense for
			// separation, I'm not really convinced it makes sense
			// for convergence. Still, the convergence override is
			// generally only useful to use convergence=0 to move
			// something to infinity, and in that case it won't
			// matter.
			set_stereo_value(state, val * saved);
		}
	} else {
		if (staging_type) {
			if (!update_val(state))
				return;
		} else
			val = expression.evaluate(state);

		COMMAND_LIST_LOG(state, "  Setting %s = %f\n", stereo_param_name(), val);
		set_stereo_value(state, val);
	}
}

bool PerDrawStereoOverrideCommand::optimise(HackerDevice *device)
{
	if (staging_type)
		return false;
	return expression.optimise(device);
}

bool PerDrawStereoOverrideCommand::noop(bool post, bool ignore_cto)
{
	NvU8 enabled = false;

	NvAPIOverride();
	NvAPI_Stereo_IsEnabled(&enabled);
	return !enabled;
}

void DirectModeSetActiveEyeCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (NVAPI_OK != NvAPI_Stereo_SetActiveEye(state->mHackerDevice->mStereoHandle, eye))
		COMMAND_LIST_LOG(state, "  Stereo_SetActiveEye failed\n");
}

bool DirectModeSetActiveEyeCommand::noop(bool post, bool ignore_cto)
{
	NvU8 enabled = false;

	NvAPIOverride();
	NvAPI_Stereo_IsEnabled(&enabled);
	return !enabled;

	// FIXME: Should also return false if direct mode is disabled...
	// if only nvapi provided a GetDriverMode() API to determine that
}

float PerDrawSeparationOverrideCommand::get_stereo_value(CommandListState *state)
{
	float ret = 0.0f;

	if (NVAPI_OK != NvAPI_Stereo_GetSeparation(state->mHackerDevice->mStereoHandle, &ret))
		COMMAND_LIST_LOG(state, "  Stereo_GetSeparation failed\n");

	return ret;
}

void PerDrawSeparationOverrideCommand::set_stereo_value(CommandListState *state, float val)
{
	NvAPIOverride();
	if (NVAPI_OK != NvAPI_Stereo_SetSeparation(state->mHackerDevice->mStereoHandle, val))
		COMMAND_LIST_LOG(state, "  Stereo_SetSeparation failed\n");
}

float PerDrawConvergenceOverrideCommand::get_stereo_value(CommandListState *state)
{
	float ret = 0.0f;

	if (NVAPI_OK != NvAPI_Stereo_GetConvergence(state->mHackerDevice->mStereoHandle, &ret))
		COMMAND_LIST_LOG(state, "  Stereo_GetConvergence failed\n");

	return ret;
}

void PerDrawConvergenceOverrideCommand::set_stereo_value(CommandListState *state, float val)
{
	NvAPIOverride();
	if (NVAPI_OK != NvAPI_Stereo_SetConvergence(state->mHackerDevice->mStereoHandle, val))
		COMMAND_LIST_LOG(state, "  Stereo_SetConvergence failed\n");
}

FrameAnalysisChangeOptionsCommand::FrameAnalysisChangeOptionsCommand(wstring *val)
{
	wchar_t *buf;
	size_t size = val->size() + 1;

	// parse_enum_option_string replaces spaces with NULLs, so it can't
	// operate on the buffer in the wstring directly. I could potentially
	// change it to work without modifying the string, but for now it's
	// easier to just make a copy of the string:
	buf = new wchar_t[size];
	wcscpy_s(buf, size, val->c_str());

	analyse_options = parse_enum_option_string<wchar_t *, FrameAnalysisOptions>
		(FrameAnalysisOptionNames, buf, NULL);

	delete [] buf;
}

void FrameAnalysisChangeOptionsCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	state->mHackerContext->FrameAnalysisTrigger(analyse_options);
}

bool FrameAnalysisChangeOptionsCommand::noop(bool post, bool ignore_cto)
{
	return (G->hunting == HUNTING_MODE_DISABLED || G->frame_analysis_registered == false);
}

static void FillInMissingInfo(ResourceCopyTargetType type, ID3D11Resource *resource, ID3D11View *view,
		UINT *stride, UINT *offset, UINT *buf_size, DXGI_FORMAT *format)
{
	D3D11_RESOURCE_DIMENSION dimension;
	D3D11_BUFFER_DESC buf_desc;
	ID3D11Buffer *buffer;

	ID3D11ShaderResourceView *resource_view = NULL;
	ID3D11RenderTargetView *render_view = NULL;
	ID3D11DepthStencilView *depth_view = NULL;
	ID3D11UnorderedAccessView *unordered_view = NULL;

	D3D11_SHADER_RESOURCE_VIEW_DESC resource_view_desc;
	D3D11_RENDER_TARGET_VIEW_DESC render_view_desc;
	D3D11_DEPTH_STENCIL_VIEW_DESC depth_view_desc;
	D3D11_UNORDERED_ACCESS_VIEW_DESC unordered_view_desc;

	ID3D11Texture1D *tex1d;
	ID3D11Texture2D *tex2d;
	ID3D11Texture3D *tex3d;
	D3D11_TEXTURE1D_DESC tex1d_desc;
	D3D11_TEXTURE2D_DESC tex2d_desc;
	D3D11_TEXTURE3D_DESC tex3d_desc;

	// Some of these may already be filled in when getting the resource
	// (either because it is stored in the pipeline state and retrieved
	// with the resource, or was stored in a custom resource). If they are
	// not we will try to fill them in here from either the resource or
	// view description as they may be necessary later to create a
	// compatible view or perform a region copy:

	resource->GetType(&dimension);
	if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
		buffer = (ID3D11Buffer*)resource;
		buffer->GetDesc(&buf_desc);
		if (*buf_size)
			*buf_size = min(*buf_size, buf_desc.ByteWidth);
		else
			*buf_size = buf_desc.ByteWidth;

		if (!*stride)
			*stride = buf_desc.StructureByteStride;
	}

	if (view) {
		switch (type) {
			case ResourceCopyTargetType::SHADER_RESOURCE:
				resource_view = (ID3D11ShaderResourceView*)view;
				resource_view->GetDesc(&resource_view_desc);
				if (*format == DXGI_FORMAT_UNKNOWN)
					*format = resource_view_desc.Format;
				if (!*stride)
					*stride = dxgi_format_size(*format);
				if (!*offset)
					*offset = resource_view_desc.Buffer.FirstElement * *stride;
				if (!*buf_size)
					*buf_size = resource_view_desc.Buffer.NumElements * *stride + *offset;
				break;
			case ResourceCopyTargetType::RENDER_TARGET:
				render_view = (ID3D11RenderTargetView*)view;
				render_view->GetDesc(&render_view_desc);
				if (*format == DXGI_FORMAT_UNKNOWN)
					*format = render_view_desc.Format;
				if (!*stride)
					*stride = dxgi_format_size(*format);
				if (!*offset)
					*offset = render_view_desc.Buffer.FirstElement * *stride;
				if (!*buf_size)
					*buf_size = render_view_desc.Buffer.NumElements * *stride + *offset;
				break;
			case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
				depth_view = (ID3D11DepthStencilView*)view;
				depth_view->GetDesc(&depth_view_desc);
				if (*format == DXGI_FORMAT_UNKNOWN)
					*format = depth_view_desc.Format;
				if (!*stride)
					*stride = dxgi_format_size(*format);
				// Depth stencil buffers cannot be buffers
				break;
			case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
				unordered_view = (ID3D11UnorderedAccessView*)view;
				unordered_view->GetDesc(&unordered_view_desc);
				if (*format == DXGI_FORMAT_UNKNOWN)
					*format = unordered_view_desc.Format;
				if (!*stride)
					*stride = dxgi_format_size(*format);
				if (!*offset)
					*offset = unordered_view_desc.Buffer.FirstElement * *stride;
				if (!*buf_size)
					*buf_size = unordered_view_desc.Buffer.NumElements * *stride + *offset;
				break;
		}
	} else if (*format == DXGI_FORMAT_UNKNOWN) {
		// If we *still* don't know the format and it's a texture, get it from
		// the resource description. This will be the case for the back buffer
		// since that does not have a view.
		switch (dimension) {
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				tex1d = (ID3D11Texture1D*)resource;
				tex1d->GetDesc(&tex1d_desc);
				*format = tex1d_desc.Format;
				break;
			case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
				tex2d = (ID3D11Texture2D*)resource;
				tex2d->GetDesc(&tex2d_desc);
				*format = tex2d_desc.Format;
				break;
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				tex3d = (ID3D11Texture3D*)resource;
				tex3d->GetDesc(&tex3d_desc);
				*format = tex3d_desc.Format;
		}
	}

	if (!*stride) {
		// This will catch index buffers, which are not structured and
		// don't have a view, but they do have a format we can use:
		*stride = dxgi_format_size(*format);

		// This will catch constant buffers, which are not structured
		// and don't have either a view or format, so set the stride to
		// the size of the whole buffer:
		if (!*stride)
			*stride = *buf_size;
	}
}

void FrameAnalysisDumpCommand::run(CommandListState *state)
{
	ID3D11Resource *resource = NULL;
	ID3D11View *view = NULL;
	UINT stride = 0;
	UINT offset = 0;
	UINT buf_size = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

	// Fast exit if frame analysis is currently inactive:
	if (!G->analyse_frame)
		return;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	resource = target.GetResource(state, &view, &stride, &offset, &format, NULL);
	if (!resource) {
		COMMAND_LIST_LOG(state, "  No resource to dump\n");
		return;
	}

	// Fill in any missing info before handing it to frame analysis. The
	// format is particularly important to try to avoid saving TYPELESS
	// resources:
	FillInMissingInfo(target.type, resource, view, &stride, &offset, &buf_size, &format);

	state->mHackerContext->FrameAnalysisDump(resource, analyse_options, target_name.c_str(), format, stride, offset);

	if (resource)
		resource->Release();
	if (view)
		view->Release();
}

bool FrameAnalysisDumpCommand::noop(bool post, bool ignore_cto)
{
	return (G->hunting == HUNTING_MODE_DISABLED || G->frame_analysis_registered == false);
}

UpscalingFlipBBCommand::UpscalingFlipBBCommand(wstring section) :
	ini_section(section)
{
	G->upscaling_command_list_using_explicit_bb_flip = true;
}

UpscalingFlipBBCommand::~UpscalingFlipBBCommand()
{
	G->upscaling_command_list_using_explicit_bb_flip = false;
}

void UpscalingFlipBBCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "[%S] special = upscaling_switch_bb\n", ini_section.c_str());

	G->bb_is_upscaling_bb = false;
}

CustomShader::CustomShader() :
	vs_override(false), hs_override(false), ds_override(false),
	gs_override(false), ps_override(false), cs_override(false),
	vs(NULL), hs(NULL), ds(NULL), gs(NULL), ps(NULL), cs(NULL),
	vs_bytecode(NULL), hs_bytecode(NULL), ds_bytecode(NULL),
	gs_bytecode(NULL), ps_bytecode(NULL), cs_bytecode(NULL),
	blend_override(0), blend_state(NULL),
	blend_sample_mask(0xffffffff), blend_sample_mask_merge_mask(0xffffffff),
	depth_stencil_override(0), depth_stencil_state(NULL),
	stencil_ref(0), stencil_ref_mask(~0),
	rs_override(0), rs_state(NULL),
	topology(D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED),
	substantiated(false),
	max_executions_per_frame(0),
	frame_no(0),
	executions_this_frame(0),
	sampler_override(0),
	sampler_state(nullptr),
	compile_flags(D3DCompileFlags::OPTIMIZATION_LEVEL3)
{
	int i;

	for (i = 0; i < 4; i++) {
		blend_factor[i] = 1.0f;
		blend_factor_merge_mask[i] = ~0;
	}
}

CustomShader::~CustomShader()
{
	if (vs)
		vs->Release();
	if (hs)
		hs->Release();
	if (ds)
		ds->Release();
	if (gs)
		gs->Release();
	if (ps)
		ps->Release();
	if (cs)
		cs->Release();

	if (blend_state)
		blend_state->Release();
	if (depth_stencil_state)
		depth_stencil_state->Release();
	if (rs_state)
		rs_state->Release();

	if (vs_bytecode)
		vs_bytecode->Release();
	if (hs_bytecode)
		hs_bytecode->Release();
	if (ds_bytecode)
		ds_bytecode->Release();
	if (gs_bytecode)
		gs_bytecode->Release();
	if (ps_bytecode)
		ps_bytecode->Release();
	if (cs_bytecode)
		cs_bytecode->Release();
	if (sampler_state)
		sampler_state->Release();
}

static const D3D_SHADER_MACRO vs_macros[] = { "VERTEX_SHADER", "", NULL, NULL };
static const D3D_SHADER_MACRO hs_macros[] = { "HULL_SHADER", "", NULL, NULL };
static const D3D_SHADER_MACRO ds_macros[] = { "DOMAIN_SHADER", "", NULL, NULL };
static const D3D_SHADER_MACRO gs_macros[] = { "GEOMETRY_SHADER", "", NULL, NULL };
static const D3D_SHADER_MACRO ps_macros[] = { "PIXEL_SHADER", "", NULL, NULL };
static const D3D_SHADER_MACRO cs_macros[] = { "COMPUTE_SHADER", "", NULL, NULL };

// This is similar to the other compile routines, but still distinct enough to
// get it's own function for now - TODO: Refactor out the common code
bool CustomShader::compile(char type, wchar_t *filename, const wstring *wname, const wstring *namespace_path)
{
	wchar_t wpath[MAX_PATH];
	char apath[MAX_PATH];
	HANDLE f;
	DWORD srcDataSize, readSize;
	vector<char> srcData;
	HRESULT hr;
	char shaderModel[7];
	ID3DBlob **ppBytecode = NULL;
	ID3DBlob *pErrorMsgs = NULL;
	const D3D_SHADER_MACRO *macros = NULL;
	bool found = false;

	LogInfo("  %cs=%S\n", type, filename);

	switch(type) {
		case 'v':
			ppBytecode = &vs_bytecode;
			macros = vs_macros;
			vs_override = true;
			break;
		case 'h':
			ppBytecode = &hs_bytecode;
			macros = hs_macros;
			hs_override = true;
			break;
		case 'd':
			ppBytecode = &ds_bytecode;
			macros = ds_macros;
			ds_override = true;
			break;
		case 'g':
			ppBytecode = &gs_bytecode;
			macros = gs_macros;
			gs_override = true;
			break;
		case 'p':
			ppBytecode = &ps_bytecode;
			macros = ps_macros;
			ps_override = true;
			break;
		case 'c':
			ppBytecode = &cs_bytecode;
			macros = cs_macros;
			cs_override = true;
			break;
		default:
			// Should not happen
			LogOverlay(LOG_DIRE, "CustomShader::compile: invalid shader type\n");
			goto err;
	}

	// Special value to unbind the shader instead:
	if (!_wcsicmp(filename, L"null"))
		return false;

	// If this section was not in the main d3dx.ini, look
	// for a file relative to the config it came from
	// first, then try relative to the 3DMigoto directory:
	found = false;
	if (!namespace_path->empty()) {
		GetModuleFileName(0, wpath, MAX_PATH);
		wcsrchr(wpath, L'\\')[1] = 0;
		wcscat(wpath, namespace_path->c_str());
		wcscat(wpath, filename);
		if (GetFileAttributes(wpath) != INVALID_FILE_ATTRIBUTES)
			found = true;
	}
	if (!found) {
		if (!GetModuleFileName(0, wpath, MAX_PATH)) {
			LogOverlay(LOG_DIRE, "CustomShader::compile: GetModuleFileName failed\n");
			goto err;
		}
		wcsrchr(wpath, L'\\')[1] = 0;
		wcscat(wpath, filename);
	}

	f = CreateFile(wpath, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		LogOverlay(LOG_WARNING, "Shader not found: %S\n", wpath);
		goto err;
	}

	srcDataSize = GetFileSize(f, 0);
	srcData.resize(srcDataSize);

	if (!ReadFile(f, srcData.data(), srcDataSize, &readSize, 0)
			|| srcDataSize != readSize) {
		LogInfo("    Error reading HLSL file\n");
		goto err_close;
	}
	CloseHandle(f);

	// Currently always using shader model 5, could allow this to be
	// overridden in the future:
	_snprintf_s(shaderModel, 7, 7, "%cs_5_0", type);

	// TODO: Add #defines for StereoParams and IniParams. Define a macro
	// for the type of shader, and maybe allow more defines to be specified
	// in the ini

	// Pass the real filename and use the standard include handler so that
	// #include will work with a relative path from the shader itself.
	// Later we could add a custom include handler to track dependencies so
	// that we can make reloading work better when using includes:
	wcstombs(apath, wpath, MAX_PATH);
	hr = D3DCompile(srcData.data(), srcDataSize, apath, macros, D3D_COMPILE_STANDARD_FILE_INCLUDE,
		"main", shaderModel, (UINT)compile_flags, 0, ppBytecode, &pErrorMsgs);

	if (pErrorMsgs) {
		LPVOID errMsg = pErrorMsgs->GetBufferPointer();
		SIZE_T errSize = pErrorMsgs->GetBufferSize();
		LogInfo("--------------------------------------------- BEGIN ---------------------------------------------\n");
		LogOverlay(LOG_NOTICE, "%*s\n", errSize, errMsg);
		LogInfo("---------------------------------------------- END ----------------------------------------------\n");
		pErrorMsgs->Release();
	}

	if (FAILED(hr)) {
		LogOverlay(LOG_WARNING, "Error compiling custom shader\n");
		goto err;
	}

	// TODO: Cache bytecode

	return false;
err_close:
	CloseHandle(f);
err:
	return true;
}

void CustomShader::substantiate(ID3D11Device *mOrigDevice1)
{
	if (substantiated)
		return;
	substantiated = true;

	if (vs_bytecode) {
		mOrigDevice1->CreateVertexShader(vs_bytecode->GetBufferPointer(), vs_bytecode->GetBufferSize(), NULL, &vs);
		CleanupShaderMaps(vs);
		vs_bytecode->Release();
		vs_bytecode = NULL;
	}
	if (hs_bytecode) {
		mOrigDevice1->CreateHullShader(hs_bytecode->GetBufferPointer(), hs_bytecode->GetBufferSize(), NULL, &hs);
		CleanupShaderMaps(hs);
		hs_bytecode->Release();
		hs_bytecode = NULL;
	}
	if (ds_bytecode) {
		mOrigDevice1->CreateDomainShader(ds_bytecode->GetBufferPointer(), ds_bytecode->GetBufferSize(), NULL, &ds);
		CleanupShaderMaps(ds);
		ds_bytecode->Release();
		ds_bytecode = NULL;
	}
	if (gs_bytecode) {
		mOrigDevice1->CreateGeometryShader(gs_bytecode->GetBufferPointer(), gs_bytecode->GetBufferSize(), NULL, &gs);
		CleanupShaderMaps(gs);
		gs_bytecode->Release();
		gs_bytecode = NULL;
	}
	if (ps_bytecode) {
		mOrigDevice1->CreatePixelShader(ps_bytecode->GetBufferPointer(), ps_bytecode->GetBufferSize(), NULL, &ps);
		CleanupShaderMaps(ps);
		ps_bytecode->Release();
		ps_bytecode = NULL;
	}
	if (cs_bytecode) {
		mOrigDevice1->CreateComputeShader(cs_bytecode->GetBufferPointer(), cs_bytecode->GetBufferSize(), NULL, &cs);
		CleanupShaderMaps(cs);
		cs_bytecode->Release();
		cs_bytecode = NULL;
	}

	if (blend_override == 1) // 2 will merge the blend state at draw time
		mOrigDevice1->CreateBlendState(&blend_desc, &blend_state);

	if (depth_stencil_override == 1) // 2 will merge depth/stencil state at draw time
		mOrigDevice1->CreateDepthStencilState(&depth_stencil_desc, &depth_stencil_state);

	if (rs_override == 1) // 2 will merge rasterizer state at draw time
		mOrigDevice1->CreateRasterizerState(&rs_desc, &rs_state);

	if (sampler_override == 1)
		mOrigDevice1->CreateSamplerState(&sampler_desc, &sampler_state);
}

// Similar to memcpy, but also takes a mask. Any bits in the mask that are set
// to 0 will be unchanged in the destination, while bits that are set to 1 will
// be copied from the source buffer.
static void memcpy_masked_merge(void *dest, void *src, void *mask, size_t n)
{
	char *c_dest = (char*)dest;
	char *c_src = (char*)src;
	char *c_mask = (char*)mask;
	size_t i;

	for (i = 0; i < n; i++)
		c_dest[i] = c_dest[i] & ~c_mask[i] | c_src[i] & c_mask[i];
}

void CustomShader::merge_blend_states(ID3D11BlendState *src_state, FLOAT src_blend_factor[4], UINT src_sample_mask, ID3D11Device *mOrigDevice1)
{
	D3D11_BLEND_DESC src_desc;
	int i;

	if (blend_override != 2)
		return;

	if (blend_state)
		blend_state->Release();
	blend_state = NULL;

	if (src_state) {
		src_state->GetDesc(&src_desc);
	} else {
		// There is no state set, so DX will be using defaults. Set the
		// source description to the defaults so the merge will still
		// work as expected:
		src_desc.AlphaToCoverageEnable = FALSE;
		src_desc.IndependentBlendEnable = FALSE;
		for (i = 0; i < 8; i++) {
			src_desc.RenderTarget[i].BlendEnable = FALSE;
			src_desc.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;
			src_desc.RenderTarget[i].DestBlend = D3D11_BLEND_ZERO;
			src_desc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
			src_desc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
			src_desc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
			src_desc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			src_desc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
	}

	memcpy_masked_merge(&blend_desc, &src_desc, &blend_mask, sizeof(D3D11_BLEND_DESC));

	for (i = 0; i < 4; i++) {
		if (blend_factor_merge_mask[i])
			blend_factor[i] = src_blend_factor[i];
	}
	blend_sample_mask = blend_sample_mask & ~blend_sample_mask_merge_mask | src_sample_mask & blend_sample_mask_merge_mask;

	mOrigDevice1->CreateBlendState(&blend_desc, &blend_state);
}

void CustomShader::merge_depth_stencil_states(ID3D11DepthStencilState *src_state, UINT src_stencil_ref, ID3D11Device *mOrigDevice1)
{
	D3D11_DEPTH_STENCIL_DESC src_desc;

	if (depth_stencil_override != 2)
		return;

	if (depth_stencil_state)
		depth_stencil_state->Release();
	depth_stencil_state = NULL;

	if (src_state) {
		src_state->GetDesc(&src_desc);
	} else {
		// There is no state set, so DX will be using defaults. Set the
		// source description to the defaults so the merge will still
		// work as expected:
		src_desc.DepthEnable = TRUE;
		src_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		src_desc.DepthFunc = D3D11_COMPARISON_LESS;
		src_desc.StencilEnable = FALSE;
		src_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
		src_desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
		src_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		src_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		src_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		src_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		src_desc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		src_desc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		src_desc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		src_desc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	}

	memcpy_masked_merge(&depth_stencil_desc, &src_desc, &depth_stencil_mask, sizeof(D3D11_DEPTH_STENCIL_DESC));
	stencil_ref = stencil_ref & ~stencil_ref_mask | src_stencil_ref & stencil_ref_mask;

	mOrigDevice1->CreateDepthStencilState(&depth_stencil_desc, &depth_stencil_state);
}

void CustomShader::merge_rasterizer_states(ID3D11RasterizerState *src_state, ID3D11Device *mOrigDevice1)
{
	D3D11_RASTERIZER_DESC src_desc;

	if (rs_override != 2)
		return;

	if (rs_state)
		rs_state->Release();
	rs_state = NULL;

	if (src_state) {
		src_state->GetDesc(&src_desc);
	} else {
		// There is no state set, so DX will be using defaults. Set the
		// source description to the defaults so the merge will still
		// work as expected:
		src_desc.FillMode = D3D11_FILL_SOLID;
		src_desc.CullMode = D3D11_CULL_BACK;
		src_desc.FrontCounterClockwise = FALSE;
		src_desc.DepthBias = 0;
		src_desc.SlopeScaledDepthBias = 0.0f;
		src_desc.DepthBiasClamp = 0.0f;
		src_desc.DepthClipEnable = TRUE;
		src_desc.ScissorEnable = FALSE;
		src_desc.MultisampleEnable = FALSE;
		src_desc.AntialiasedLineEnable = FALSE;
	}

	memcpy_masked_merge(&rs_desc, &src_desc, &rs_mask, sizeof(D3D11_RASTERIZER_DESC));

	mOrigDevice1->CreateRasterizerState(&rs_desc, &rs_state);
}

struct saved_shader_inst
{
	ID3D11ClassInstance *instances[256];
	UINT num_instances;

	saved_shader_inst() :
		num_instances(0)
	{}

	~saved_shader_inst()
	{
		UINT i;

		for (i = 0; i < num_instances; i++) {
			if (instances[i])
				instances[i]->Release();
		}
	}
};

void RunCustomShaderCommand::run(CommandListState *state)
{
	ID3D11Device *mOrigDevice1 = state->mOrigDevice1;
	ID3D11DeviceContext *mOrigContext1 = state->mOrigContext1;
	ID3D11VertexShader *saved_vs = NULL;
	ID3D11HullShader *saved_hs = NULL;
	ID3D11DomainShader *saved_ds = NULL;
	ID3D11GeometryShader *saved_gs = NULL;
	ID3D11PixelShader *saved_ps = NULL;
	ID3D11ComputeShader *saved_cs = NULL;
	ID3D11BlendState *saved_blend = NULL;
	ID3D11DepthStencilState *saved_depth_stencil = NULL;
	ID3D11RasterizerState *saved_rs = NULL;
	UINT num_viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT saved_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	FLOAT saved_blend_factor[4];
	UINT saved_sample_mask;
	UINT saved_stencil_ref;
	bool saved_post;
	struct OMState om_state;
	UINT i;
	D3D11_PRIMITIVE_TOPOLOGY saved_topology;
	UINT num_sampler = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
	ID3D11SamplerState* saved_sampler_states[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];

	for (UINT i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i)
	{
		saved_sampler_states[i] = nullptr;
	}

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (custom_shader->max_executions_per_frame) {
		if (custom_shader->frame_no != G->frame_no) {
			custom_shader->frame_no = G->frame_no;
			custom_shader->executions_this_frame = 1;
		} else if (custom_shader->executions_this_frame++ >= custom_shader->max_executions_per_frame) {
			COMMAND_LIST_LOG(state, "  max_executions_per_frame exceeded\n");
			return;
		}
	}

	custom_shader->substantiate(mOrigDevice1);

	saved_shader_inst vs_inst, hs_inst, ds_inst, gs_inst, ps_inst, cs_inst;

	// Assign custom shaders first before running the command lists, and
	// restore them last. This is so that if someone was injecting a
	// sequence of pixel shaders that all shared a common vertex shader
	// we can avoid having to repeatedly save & restore the vertex shader
	// by calling the next shader in sequence from the command list after
	// the draw call.

	if (custom_shader->vs_override) {
		mOrigContext1->VSGetShader(&saved_vs, vs_inst.instances, &vs_inst.num_instances);
		mOrigContext1->VSSetShader(custom_shader->vs, NULL, 0);
	}
	if (custom_shader->hs_override) {
		mOrigContext1->HSGetShader(&saved_hs, hs_inst.instances, &hs_inst.num_instances);
		mOrigContext1->HSSetShader(custom_shader->hs, NULL, 0);
	}
	if (custom_shader->ds_override) {
		mOrigContext1->DSGetShader(&saved_ds, ds_inst.instances, &ds_inst.num_instances);
		mOrigContext1->DSSetShader(custom_shader->ds, NULL, 0);
	}
	if (custom_shader->gs_override) {
		mOrigContext1->GSGetShader(&saved_gs, gs_inst.instances, &gs_inst.num_instances);
		mOrigContext1->GSSetShader(custom_shader->gs, NULL, 0);
	}
	if (custom_shader->ps_override) {
		mOrigContext1->PSGetShader(&saved_ps, ps_inst.instances, &ps_inst.num_instances);
		mOrigContext1->PSSetShader(custom_shader->ps, NULL, 0);
	}
	if (custom_shader->cs_override) {
		mOrigContext1->CSGetShader(&saved_cs, cs_inst.instances, &cs_inst.num_instances);
		mOrigContext1->CSSetShader(custom_shader->cs, NULL, 0);
	}
	if (custom_shader->blend_override) {
		mOrigContext1->OMGetBlendState(&saved_blend, saved_blend_factor, &saved_sample_mask);
		custom_shader->merge_blend_states(saved_blend, saved_blend_factor, saved_sample_mask, mOrigDevice1);
		mOrigContext1->OMSetBlendState(custom_shader->blend_state, custom_shader->blend_factor, custom_shader->blend_sample_mask);
	}
	if (custom_shader->depth_stencil_override) {
		mOrigContext1->OMGetDepthStencilState(&saved_depth_stencil, &saved_stencil_ref);
		custom_shader->merge_depth_stencil_states(saved_depth_stencil, saved_stencil_ref, mOrigDevice1);
		mOrigContext1->OMSetDepthStencilState(custom_shader->depth_stencil_state, custom_shader->stencil_ref);
	}
	if (custom_shader->rs_override) {
		mOrigContext1->RSGetState(&saved_rs);
		custom_shader->merge_rasterizer_states(saved_rs, mOrigDevice1);
		mOrigContext1->RSSetState(custom_shader->rs_state);
	}
	if (custom_shader->topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
		mOrigContext1->IAGetPrimitiveTopology(&saved_topology);
		mOrigContext1->IASetPrimitiveTopology(custom_shader->topology);
	}
	if (custom_shader->sampler_override) {
		mOrigContext1->PSGetSamplers(0, num_sampler, saved_sampler_states);
		mOrigContext1->PSSetSamplers(0, 1, &custom_shader->sampler_state); //just one slot for the moment TODO: allow more via *.ini file
	}
	// We save off the viewports unconditionally for now. We could
	// potentially skip this by flagging if a command list may alter them,
	// but that probably wouldn't buy us anything:
	mOrigContext1->RSGetViewports(&num_viewports, saved_viewports);
	// Likewise, save off all RTVs, UAVs and DSVs unconditionally:
	save_om_state(state->mOrigContext1, &om_state);

	// Run the command lists. This should generally include a draw or
	// dispatch call, or call out to another command list which does.
	// The reason for having a post command list is so that people can
	// write 'ps-t100 = ResourceFoo; post ps-t100 = null' and have it work.
	saved_post = state->post;
	state->post = false;
	_RunCommandList(&custom_shader->command_list, state);
	state->post = true;
	_RunCommandList(&custom_shader->post_command_list, state);
	state->post = saved_post;

	// Finally restore the original shaders
	if (custom_shader->vs_override)
		mOrigContext1->VSSetShader(saved_vs, vs_inst.instances, vs_inst.num_instances);
	if (custom_shader->hs_override)
		mOrigContext1->HSSetShader(saved_hs, hs_inst.instances, hs_inst.num_instances);
	if (custom_shader->ds_override)
		mOrigContext1->DSSetShader(saved_ds, ds_inst.instances, ds_inst.num_instances);
	if (custom_shader->gs_override)
		mOrigContext1->GSSetShader(saved_gs, gs_inst.instances, gs_inst.num_instances);
	if (custom_shader->ps_override)
		mOrigContext1->PSSetShader(saved_ps, ps_inst.instances, ps_inst.num_instances);
	if (custom_shader->cs_override)
		mOrigContext1->CSSetShader(saved_cs, cs_inst.instances, cs_inst.num_instances);
	if (custom_shader->blend_override)
		mOrigContext1->OMSetBlendState(saved_blend, saved_blend_factor, saved_sample_mask);
	if (custom_shader->depth_stencil_override)
		mOrigContext1->OMSetDepthStencilState(saved_depth_stencil, saved_stencil_ref);
	if (custom_shader->rs_override)
		mOrigContext1->RSSetState(saved_rs);
	if (custom_shader->topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
		mOrigContext1->IASetPrimitiveTopology(saved_topology);
	if (custom_shader->sampler_override)
		mOrigContext1->PSSetSamplers(0, num_sampler, saved_sampler_states);

	mOrigContext1->RSSetViewports(num_viewports, saved_viewports);
	restore_om_state(mOrigContext1, &om_state);

	if (saved_vs)
		saved_vs->Release();
	if (saved_hs)
		saved_hs->Release();
	if (saved_ds)
		saved_ds->Release();
	if (saved_gs)
		saved_gs->Release();
	if (saved_ps)
		saved_ps->Release();
	if (saved_cs)
		saved_cs->Release();
	if (saved_blend)
		saved_blend->Release();
	if (saved_depth_stencil)
		saved_depth_stencil->Release();
	if (saved_rs)
		saved_rs->Release();

	for (i = 0; i < num_sampler; ++i) {
		if (saved_sampler_states[i])
			saved_sampler_states[i]->Release();
	}
}

bool RunCustomShaderCommand::noop(bool post, bool ignore_cto)
{
	return (custom_shader->command_list.commands.empty() && custom_shader->post_command_list.commands.empty());
}

void RunExplicitCommandList::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (state->post)
		_RunCommandList(&command_list_section->post_command_list, state);
	else
		_RunCommandList(&command_list_section->command_list, state);
}

bool RunExplicitCommandList::noop(bool post, bool ignore_cto)
{
	if (post)
		return command_list_section->post_command_list.commands.empty();
	return command_list_section->command_list.commands.empty();
}

void LinkCommandLists(CommandList *dst, CommandList *link, const wstring *ini_line)
{
	RunLinkedCommandList *operation = new RunLinkedCommandList(link);
	operation->ini_line = *ini_line;
	dst->commands.push_back(std::shared_ptr<CommandListCommand>(operation));
}

void RunLinkedCommandList::run(CommandListState *state)
{
	_RunCommandList(link, state);
}

bool RunLinkedCommandList::noop(bool post, bool ignore_cto)
{
	return link->commands.empty();
}

static void ProcessParamRTSize(CommandListState *state)
{
	D3D11_RENDER_TARGET_VIEW_DESC view_desc;
	D3D11_TEXTURE2D_DESC res_desc;
	ID3D11RenderTargetView *view = NULL;
	ID3D11Resource *res = NULL;
	ID3D11Texture2D *tex = NULL;

	if (state->rt_width != -1)
		return;

	state->mOrigContext1->OMGetRenderTargets(1, &view, NULL);
	if (!view)
		return;

	view->GetDesc(&view_desc);

	if (view_desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D &&
	    view_desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS)
		goto out_release_view;

	view->GetResource(&res);
	if (!res)
		goto out_release_view;

	tex = (ID3D11Texture2D *)res;
	tex->GetDesc(&res_desc);

	state->rt_width = (float)res_desc.Width;
	state->rt_height = (float)res_desc.Height;

	tex->Release();
out_release_view:
	view->Release();
}

static void UpdateScissorInfo(CommandListState *state)
{
	UINT num = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

	if (state->scissor_valid)
		return;

	state->mOrigContext1->RSGetScissorRects(&num, state->scissor_rects);

	state->scissor_valid = true;
}

float CommandListOperand::process_texture_filter(CommandListState *state)
{
	TextureOverrideMatches matches;
	TextureOverrideMatches::reverse_iterator rit;
	bool resource_found;

	texture_filter_target.FindTextureOverrides(state, &resource_found, &matches);

	// If there is no resource bound we want to return a special value that
	// is distinct from simply not finding a texture override section. For
	// backwards compatibility we use negative zero -0.0, because any
	// existing fixes that test for zero/non-zero to check if a matching
	// [TextureOverride] is present would expect an unbound texture to
	// never have a hash and therefore be equal to 0, and -0.0 *is* equal
	// to +0, so these will continue to work. To explicitly test for an
	// unassigned resource, use this HLSL to reinterpret the values as
	// integers and check the sign bit:
	//
	// if (asint(IniParams[0].x) == asint(-0.0)) { ... }
	//
	if (!resource_found)
		return -0.0;

	// A resource was bound, but no matching texture override was found:
	if (matches.empty())
		return 0;

	// If there are multiple matches, we want the filter_index with the
	// highest priority, which will be the last in the list that has a
	// filter index. In the future we may also want a namespaced version of
	// this (and checktextureoverride) to limit the check to sections
	// appearing in the same namespace or with a given prefix (but we don't
	// want to do string processing on the namespace here - the candidates
	// should already be narrowed down during ini parsing):
	for (rit = matches.rbegin(); rit != matches.rend(); rit++) {
		if ((*rit)->filter_index != FLT_MAX)
			return (*rit)->filter_index;
	}

	// No match had a filter_index, but there was at least one match:
	return 1.0;
}


CommandListState::CommandListState() :
	mHackerDevice(NULL),
	mHackerContext(NULL),
	mOrigDevice1(NULL),
	mOrigContext1(NULL),
	rt_width(-1),
	rt_height(-1),
	call_info(NULL),
	resource(NULL),
	view(NULL),
	post(false),
	update_params(false),
	cursor_mask_tex(NULL),
	cursor_mask_view(NULL),
	cursor_color_tex(NULL),
	cursor_color_view(NULL),
	recursion(0),
	aborted(false),
	scissor_valid(false)
{
	memset(&cursor_info, 0, sizeof(CURSORINFO));
	memset(&cursor_info_ex, 0, sizeof(ICONINFO));
	memset(&window_rect, 0, sizeof(RECT));
}

CommandListState::~CommandListState()
{
	if (cursor_info_ex.hbmMask)
		DeleteObject(cursor_info_ex.hbmMask);
	if (cursor_info_ex.hbmColor)
		DeleteObject(cursor_info_ex.hbmColor);
	if (cursor_mask_view)
		cursor_mask_view->Release();
	if (cursor_mask_tex)
		cursor_mask_tex->Release();
	if (cursor_color_view)
		cursor_color_view->Release();
	if (cursor_color_tex)
		cursor_color_tex->Release();
}

static void UpdateWindowInfo(CommandListState *state)
{
	if (state->window_rect.right)
		return;

	if (G->hWnd)
		CursorUpscalingBypass_GetClientRect(G->hWnd, &state->window_rect);
	else
		LogDebug("UpdateWindowInfo: No hWnd\n");
}

static void UpdateCursorInfo(CommandListState *state)
{
	if (state->cursor_info.cbSize)
		return;

	state->cursor_info.cbSize = sizeof(CURSORINFO);
	CursorUpscalingBypass_GetCursorInfo(&state->cursor_info);
	memcpy(&state->cursor_window_coords, &state->cursor_info.ptScreenPos, sizeof(POINT));

	if (G->hWnd)
		CursorUpscalingBypass_ScreenToClient(G->hWnd, &state->cursor_window_coords);
	else
		LogDebug("UpdateCursorInfo: No hWnd\n");
}

static void UpdateCursorInfoEx(CommandListState *state)
{
	if (state->cursor_info_ex.hbmMask)
		return;

	UpdateCursorInfo(state);

	GetIconInfo(state->cursor_info.hCursor, &state->cursor_info_ex);
}

// Uses an undocumented Windows API to get info about animated cursors and
// calculate the current frame based on the global tick count
// https://stackoverflow.com/questions/6969801/how-do-i-determine-if-the-current-mouse-cursor-is-animated
static unsigned GetCursorFrame(HCURSOR cursor)
{
	typedef HCURSOR(WINAPI* GET_CURSOR_FRAME_INFO)(HCURSOR, LPCWSTR, DWORD, DWORD*, DWORD*);
	static GET_CURSOR_FRAME_INFO fnGetCursorFrameInfo = NULL;
	HMODULE libUser32 = NULL;
	DWORD period = 6, frames = 1;

	if (!fnGetCursorFrameInfo) {
		libUser32 = LoadLibraryA("user32.dll");
		if (!libUser32)
			return 0;

		fnGetCursorFrameInfo = (GET_CURSOR_FRAME_INFO)GetProcAddress(libUser32, "GetCursorFrameInfo");
		if (!fnGetCursorFrameInfo)
			return 0;
	}

	fnGetCursorFrameInfo(cursor, L"", 0, &period, &frames);

	// Avoid divide by zero if not an animated cursor:
	if (!period || !frames)
		return 0;

	// period is a multiple of 1/60 seconds. We should really use the ms
	// since this cursor was most recently displayed, but the global tick
	// count works well enough and means we have less state to track:
	return (GetTickCount() * 6) / (period * 100) % frames;
}

static void _CreateTextureFromBitmap(HDC dc, BITMAP *bitmap_obj,
		HBITMAP hbitmap, CommandListState *state,
		ID3D11Texture2D **tex, ID3D11ShaderResourceView **view)
{
	D3D11_SHADER_RESOURCE_VIEW_DESC rv_desc;
	BITMAPINFOHEADER bmp_info;
	D3D11_SUBRESOURCE_DATA data;
	D3D11_TEXTURE2D_DESC desc;
	HRESULT hr;

	bmp_info.biSize = sizeof(BITMAPINFOHEADER);
	bmp_info.biWidth = bitmap_obj->bmWidth;
	bmp_info.biHeight = bitmap_obj->bmHeight;
	// Requesting 32bpp here to simplify the conversion process - the
	// R1_UNORM format can't be used for the 1bpp mask because that format
	// has a special purpose, and requesting 8 or 16bpp would require an
	// array of RGBQUADs after the BITMAPINFO structure for the pallette
	// that I don't want to deal with, and there is no DXGI_FORMAT for
	// 24bpp... 32bpp should work for both the mask and palette:
	bmp_info.biBitCount = 32;
	bmp_info.biPlanes = 1;
	bmp_info.biCompression = BI_RGB;
	// Pretty sure these are ignored / output only in GetDIBits:
	bmp_info.biSizeImage = 0;
	bmp_info.biXPelsPerMeter = 0;
	bmp_info.biYPelsPerMeter = 0;
	bmp_info.biClrUsed = 0;
	bmp_info.biClrImportant = 0;

	// This padding came from an example on MSDN, but I can't find
	// the documentation that indicates exactly what it is supposed
	// to be. Since we're using 32bpp, this shouldn't matter anyway:
	data.SysMemPitch = ((bitmap_obj->bmWidth * bmp_info.biBitCount + 31) / 32) * 4;

	data.pSysMem = new char[data.SysMemPitch * bitmap_obj->bmHeight];

	if (!GetDIBits(dc, hbitmap, 0, bmp_info.biHeight,
			(LPVOID)data.pSysMem, (BITMAPINFO*)&bmp_info, DIB_RGB_COLORS)) {
		LogInfo("Software Mouse: GetDIBits() failed\n");
		goto err_free;
	}

	desc.Width = bitmap_obj->bmWidth;
	desc.Height = bitmap_obj->bmHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	// FIXME: Use DXGI_FORMAT_B8G8R8X8_UNORM_SRGB if no alpha channel (there is no API to check)
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	hr = state->mOrigDevice1->CreateTexture2D(&desc, &data, tex);
	if (FAILED(hr)) {
		LogInfo("Software Mouse: CreateTexture2D Failed: 0x%x\n", hr);
		goto err_free;
	}

	rv_desc.Format = desc.Format;
	rv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	rv_desc.Texture2D.MostDetailedMip = 0;
	rv_desc.Texture2D.MipLevels = 1;

	hr = state->mOrigDevice1->CreateShaderResourceView(*tex, &rv_desc, view);
	if (FAILED(hr)) {
		LogInfo("Software Mouse: CreateShaderResourceView Failed: 0x%x\n", hr);
		goto err_release_tex;
	}

	delete [] data.pSysMem;

	return;
err_release_tex:
	(*tex)->Release();
	*tex = NULL;
err_free:
	delete [] data.pSysMem;
}

static void CreateTextureFromBitmap(HDC dc, HBITMAP hbitmap, CommandListState *state,
		ID3D11Texture2D **tex, ID3D11ShaderResourceView **view)
{
	BITMAP bitmap_obj;

	if (!GetObject(hbitmap, sizeof(BITMAP), &bitmap_obj)) {
		LogInfo("Software Mouse: GetObject() failed\n");
		return;
	}

	_CreateTextureFromBitmap(dc, &bitmap_obj, hbitmap, state, tex, view);
}

static void CreateTextureFromAnimatedCursor(
		HDC dc,
		HCURSOR cursor,
		UINT flags,
		HBITMAP static_bitmap,
		CommandListState *state,
		ID3D11Texture2D **tex,
		ID3D11ShaderResourceView **view
		)
{
	BITMAP bitmap_obj;
	HDC dc_mem;
	HBITMAP ani_bitmap;
	unsigned frame;

	if (!GetObject(static_bitmap, sizeof(BITMAP), &bitmap_obj)) {
		LogInfo("Software Mouse: GetObject() failed\n");
		return;
	}

	dc_mem = CreateCompatibleDC(dc);
	if (!dc_mem) {
		LogInfo("Software Mouse: CreateCompatibleDC() failed\n");
		return;
	}

	ani_bitmap = CreateCompatibleBitmap(dc, bitmap_obj.bmWidth, bitmap_obj.bmHeight);
	if (!ani_bitmap) {
		LogInfo("Software Mouse: CreateCompatibleBitmap() failed\n");
		goto out_delete_mem_dc;
	}

	frame = GetCursorFrame(cursor);

	// To get a frame from an animated cursor we have to use DrawIconEx to
	// draw it to another bitmap, then we can create a texture from that
	// bitmap:
	SelectObject(dc_mem, ani_bitmap);
	if (!DrawIconEx(dc_mem, 0, 0, cursor, bitmap_obj.bmWidth, bitmap_obj.bmHeight, frame, NULL, flags)) {
		LogInfo("Software Mouse: DrawIconEx failed\n");
		// Fall back to getting the first frame from the static_bitmap we already have:
		_CreateTextureFromBitmap(dc, &bitmap_obj, static_bitmap, state, tex, view);
		goto out_delete_ani_bitmap;
	}

	_CreateTextureFromBitmap(dc, &bitmap_obj, ani_bitmap, state, tex, view);

out_delete_ani_bitmap:
	DeleteObject(ani_bitmap);
out_delete_mem_dc:
	DeleteDC(dc_mem);
}

static void UpdateCursorResources(CommandListState *state)
{
	HDC dc;

	if (state->cursor_mask_tex || state->cursor_color_tex)
		return;

	UpdateCursorInfoEx(state);

	// XXX: Should maybe be the device context for the window?
	dc = GetDC(NULL);
	if (!dc) {
		LogInfo("Software Mouse: GetDC() failed\n");
		return;
	}

	if (state->cursor_info_ex.hbmColor) {
		// Colour cursor, which may or may not be animated, but the
		// animated routine will work either way:
		CreateTextureFromAnimatedCursor(
				dc,
				state->cursor_info.hCursor,
				DI_IMAGE,
				state->cursor_info_ex.hbmColor,
				state,
				&state->cursor_color_tex,
				&state->cursor_color_view);

		if (state->cursor_info_ex.hbmMask) {
			// Since it's a colour cursor the mask bitmap will be
			// the regular height, which will work with the
			// animated routine:
			CreateTextureFromAnimatedCursor(
					dc,
					state->cursor_info.hCursor,
					DI_MASK,
					state->cursor_info_ex.hbmMask,
					state,
					&state->cursor_mask_tex,
					&state->cursor_mask_view);
		}
	} else if (state->cursor_info_ex.hbmMask) {
		// Black and white cursor, which means the hbmMask bitmap is
		// double height and won't work with the animated cursor
		// routines, so just turn the bitmap into a texture directly:
		CreateTextureFromBitmap(
				dc,
				state->cursor_info_ex.hbmMask,
				state,
				&state->cursor_mask_tex,
				&state->cursor_mask_view);
	}

	ReleaseDC(NULL, dc);
}

static bool sli_enabled(HackerDevice *device)
{
	NV_GET_CURRENT_SLI_STATE sli_state;
	sli_state.version = NV_GET_CURRENT_SLI_STATE_VER;
	NvAPI_Status status;

	status = NvAPI_D3D_GetCurrentSLIState(device->GetPossiblyHookedOrigDevice1(), &sli_state);
	if (status != NVAPI_OK) {
		LogInfo("Unable to retrieve SLI state from nvapi\n");
		return false;
	}

	return sli_state.maxNumAFRGroups > 1;
}

float CommandListOperand::evaluate(CommandListState *state, HackerDevice *device)
{
	NvU8 stereo = false;
	float fret;

	if (state)
		device = state->mHackerDevice;
	else if (!device) {
		LogInfo("BUG: CommandListOperand::evaluate called with neither state nor device\n");
		DoubleBeepExit();
	}

	// XXX: If updating this list, be sure to also update
	// XXX: operand_allowed_in_context()
	switch (type) {
		case ParamOverrideType::VALUE:
			return val;
		case ParamOverrideType::INI_PARAM:
			return G->iniParams[param_idx].*param_component;
		case ParamOverrideType::RES_WIDTH:
			return (float)G->mResolutionInfo.width;
		case ParamOverrideType::RES_HEIGHT:
			return (float)G->mResolutionInfo.height;
		case ParamOverrideType::TIME:
			return (float)(GetTickCount() - G->ticks_at_launch) / 1000.0f;
		case ParamOverrideType::RAW_SEPARATION:
			// We could use cached values of these (nvapi is known
			// to become a bottleneck with too many calls / frame),
			// but they need to be up to date, taking into account
			// any changes made via the command list already this
			// frame (this is used for snapshots and getting the
			// current convergence regardless of whether an
			// asynchronous transfer from the GPU has or has not
			// completed) - StereoParams is currently unsuitable
			// for this as it is only updated once / frame... We
			// could change it so that StereoParams is always up to
			// date - it would differ from the historical
			// behaviour, but I doubt it would break anything.
			// Otherwise we could have a separate cache. Whatever -
			// this is rarely used, so let's just go with this for
			// now and worry about optimisations only if it proves
			// to be a bottleneck in practice:
			NvAPI_Stereo_GetSeparation(device->mStereoHandle, &fret);
			return fret;
		case ParamOverrideType::CONVERGENCE:
			NvAPI_Stereo_GetConvergence(device->mStereoHandle, &fret);
			return fret;
		case ParamOverrideType::EYE_SEPARATION:
			NvAPI_Stereo_GetEyeSeparation(device->mStereoHandle, &fret);
			return fret;
		case ParamOverrideType::STEREO_ACTIVE:
			NvAPI_Stereo_IsActivated(device->mStereoHandle, &stereo);
			return !!stereo;
		case ParamOverrideType::SLI:
			return sli_enabled(device);
		// XXX: If updating this list, be sure to also update
		// XXX: operand_allowed_in_context()
	}

	if (!state) {
		// FIXME: Some of these only use the state object for cache,
		// and could still be evaluated if we forgo the cache
		LogOverlay(LOG_WARNING, "BUG: Operand type %i cannot be evaluated outside of a command list\n", type);
		return 0;
	}

	switch (type) {
		case ParamOverrideType::RT_WIDTH:
			ProcessParamRTSize(state);
			return state->rt_width;
		case ParamOverrideType::RT_HEIGHT:
			ProcessParamRTSize(state);
			return state->rt_height;
		case ParamOverrideType::WINDOW_WIDTH:
			UpdateWindowInfo(state);
			return (float)state->window_rect.right;
		case ParamOverrideType::WINDOW_HEIGHT:
			UpdateWindowInfo(state);
			return (float)state->window_rect.bottom;
		case ParamOverrideType::TEXTURE:
			return process_texture_filter(state);
		case ParamOverrideType::VERTEX_COUNT:
			if (state->call_info)
				return (float)state->call_info->VertexCount;
			return 0;
		case ParamOverrideType::INDEX_COUNT:
			if (state->call_info)
				return (float)state->call_info->IndexCount;
			return 0;
		case ParamOverrideType::INSTANCE_COUNT:
			if (state->call_info)
				return (float)state->call_info->InstanceCount;
			return 0;
		case ParamOverrideType::CURSOR_VISIBLE:
			UpdateCursorInfo(state);
			return !!(state->cursor_info.flags & CURSOR_SHOWING);
		case ParamOverrideType::CURSOR_SCREEN_X:
			UpdateCursorInfo(state);
			return (float)state->cursor_info.ptScreenPos.x;
		case ParamOverrideType::CURSOR_SCREEN_Y:
			UpdateCursorInfo(state);
			return (float)state->cursor_info.ptScreenPos.y;
		case ParamOverrideType::CURSOR_WINDOW_X:
			UpdateCursorInfo(state);
			return (float)state->cursor_window_coords.x;
		case ParamOverrideType::CURSOR_WINDOW_Y:
			UpdateCursorInfo(state);
			return (float)state->cursor_window_coords.y;
		case ParamOverrideType::CURSOR_X:
			UpdateCursorInfo(state);
			UpdateWindowInfo(state);
			return (float)state->cursor_window_coords.x / (float)state->window_rect.right;
		case ParamOverrideType::CURSOR_Y:
			UpdateCursorInfo(state);
			UpdateWindowInfo(state);
			return (float)state->cursor_window_coords.y / (float)state->window_rect.bottom;
		case ParamOverrideType::CURSOR_HOTSPOT_X:
			UpdateCursorInfoEx(state);
			return (float)state->cursor_info_ex.xHotspot;
		case ParamOverrideType::CURSOR_HOTSPOT_Y:
			UpdateCursorInfoEx(state);
			return (float)state->cursor_info_ex.yHotspot;
		case ParamOverrideType::SCISSOR_LEFT:
			UpdateScissorInfo(state);
			return (float)state->scissor_rects[scissor].left;
		case ParamOverrideType::SCISSOR_TOP:
			UpdateScissorInfo(state);
			return (float)state->scissor_rects[scissor].top;
		case ParamOverrideType::SCISSOR_RIGHT:
			UpdateScissorInfo(state);
			return (float)state->scissor_rects[scissor].right;
		case ParamOverrideType::SCISSOR_BOTTOM:
			UpdateScissorInfo(state);
			return (float)state->scissor_rects[scissor].bottom;
	}

	LogOverlay(LOG_DIRE, "BUG: Unhandled operand type %i\n", type);
	return 0;
}

bool CommandListOperand::static_evaluate(float *ret, HackerDevice *device)
{
	NvU8 stereo = false;

	switch (type) {
		case ParamOverrideType::VALUE:
			*ret = val;
			return true;
		case ParamOverrideType::RAW_SEPARATION:
		case ParamOverrideType::CONVERGENCE:
		case ParamOverrideType::EYE_SEPARATION:
		case ParamOverrideType::STEREO_ACTIVE:
			NvAPIOverride();
			NvAPI_Stereo_IsEnabled(&stereo);
			if (!stereo) {
				*ret = 0.0;
				return true;
			}
			break;
		case ParamOverrideType::SLI:
			if (device) {
				*ret = sli_enabled(device);
				return true;
			}
			break;
	}

	return false;
}

bool CommandListOperand::optimise(HackerDevice *device)
{
	if (type == ParamOverrideType::VALUE)
		return false;

	if (!static_evaluate(&val, device))
		return false;

	LogInfo("Statically evaluated %S as %f\n",
		lookup_enum_name(ParamOverrideTypeNames, type), val);

	type = ParamOverrideType::VALUE;
	return true;
}

static const wchar_t *operator_tokens[] = {
	// Two character tokens first:
	L"==", L"!=", L"//", L"<=", L">=", L"&&", L"||", L"**",
	// Single character tokens last:
	L"(", L")", L"!", L"*", L"/", L"%", L"+", L"-", L"<", L">",
#if 0
	// Proposed operator precedence
	L"(", L")",
	L"**", /* Exponential (Right associative) */
	L"!", /* Unary+, Unary- (Right associative) */
	L"*", L"//", L"/", L"%",
	L"+", L"-", /* Addition, Subtraction */
	L"<=", L">=", L"<", L">",
	L"==", L"!=",
	L"&&",
	L"||",
#endif
};

class CommandListSyntaxError: public exception
{
public:
	wstring msg;
	size_t pos;

	CommandListSyntaxError(wstring msg, size_t pos) :
		msg(msg), pos(pos)
	{}
};

static void tokenise(const wstring *expression, CommandListSyntaxTree *tree, const wstring *ini_namespace, bool command_list_context)
{
	wstring remain = *expression;
	ResourceCopyTarget texture_filter_target;
	shared_ptr<CommandListOperand> operand;
	wstring token;
	size_t pos = 0;
	size_t friendly_pos = 0;
	float fval;
	int ret;
	int i;
	bool last_was_operand = false;

	LogInfo("    Tokenising \"%S\"\n", expression->c_str());

	while (true) {
next_token:
		// Skip whitespace:
		pos = remain.find_first_not_of(L" \t", pos);
		if (pos == wstring::npos)
			return;
		remain = remain.substr(pos);
		friendly_pos += pos;

		// Operators:
		for (i = 0; i < ARRAYSIZE(operator_tokens); i++) {
			if (!remain.compare(0, wcslen(operator_tokens[i]), operator_tokens[i])) {
				pos = wcslen(operator_tokens[i]);
				tree->tokens.emplace_back(make_shared<CommandListOperatorToken>(friendly_pos, remain.substr(0, pos)));
				LogInfo("      Operator: \"%S\"\n", tree->tokens.back()->token.c_str());
				last_was_operand = false;
				goto next_token; // continue would continue wrong loop
			}
		}

		// Texture Filtering / Resource Slots:
		// - Many of these slots include a hyphen character, which
		//   conflicts with the subtraction/negation operators,
		//   potentially making something like "x = ps-t0" ambiguous as
		//   to whether it is referring to pixel shader texture slot 0,
		//   or subtracting "t0" from "ps", but in practice this should
		//   be generally be fine since we don't have anything called
		//   "ps", "t0" or similar, and if we did simply adding
		//   whitespace around the subtraction would disambiguate it.
		// - The characters we check for here preclude some arbitrary
		//   custom Resource names, including namespaced resources, but
		//   that's ok since this is only for texture filtering, which
		//   doesn't work if custom resources are checked. If we need
		//   to match these for some other reason, we could add \ and .
		//   to this list, which will cover most namespaced resources.
		pos = remain.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz_-0123456789");
		if (pos) {
			token = remain.substr(0, pos);
			ret = texture_filter_target.ParseTarget(token.c_str(), true, ini_namespace);
			if (ret) {
				operand = make_shared<CommandListOperand>(friendly_pos, token);
				if (operand->parse(&token, ini_namespace, command_list_context)) {
					tree->tokens.emplace_back(std::move(operand));
					LogInfo("      Resource Slot: \"%S\"\n", tree->tokens.back()->token.c_str());
					if (last_was_operand)
						throw CommandListSyntaxError(L"Unexpected identifier", friendly_pos);
					last_was_operand = true;
					continue;
				} else {
					LogOverlay(LOG_DIRE, "BUG: Token parsed as resource slot, but not as operand: %S\n", token.c_str());
					DoubleBeepExit();
				}
			}
		}

		// Identifiers:
		// - Parse this before floats to make sure that the special
		//   cases "inf" and "nan" are identifiers by themselves, not
		//   the start of some other identifier. Only applies to
		//   vs2015+ as older toolchains lack parsing for these.
		// - Identifiers cannot start with a number
		if (remain[0] < '0' || remain[0] > '9') {
			pos = remain.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz_0123456789");
			if (pos) {
				token = remain.substr(0, pos);
				operand = make_shared<CommandListOperand>(friendly_pos, token);
				if (operand->parse(&token, ini_namespace, command_list_context)) {
					tree->tokens.emplace_back(std::move(operand));
					LogInfo("      Identifier: \"%S\"\n", tree->tokens.back()->token.c_str());
					if (last_was_operand)
						throw CommandListSyntaxError(L"Unexpected identifier", friendly_pos);
					last_was_operand = true;
					continue;
				}
				throw CommandListSyntaxError(L"Unrecognised identifier: " + token, friendly_pos);
			}
		}

		// Floats:
		// - Must tokenise subtraction operation first
		//   - Static optimisation will merge unary negation
		// - Identifier match will catch "nan" and "inf" special cases
		//   if the toolchain supports them
		ret = swscanf_s(remain.c_str(), L"%f%zn", &fval, &pos);
		if (ret != 0 && ret != EOF) {
			token = remain.substr(0, pos);
			operand = make_shared<CommandListOperand>(friendly_pos, token);
			if (operand->parse(&token, ini_namespace, command_list_context)) {
				tree->tokens.emplace_back(std::move(operand));
				LogInfo("      Float: \"%S\"\n", tree->tokens.back()->token.c_str());
				if (last_was_operand)
					throw CommandListSyntaxError(L"Unexpected identifier", friendly_pos);
				last_was_operand = true;
				continue;
			} else {
				LogOverlay(LOG_DIRE, "BUG: Token parsed as float, but not as operand: %S\n", token.c_str());
				DoubleBeepExit();
			}
		}

		throw CommandListSyntaxError(L"Parse error", friendly_pos);
	}
}

static void group_parenthesis(CommandListSyntaxTree *tree)
{
	CommandListSyntaxTree::Tokens::iterator i;
	CommandListSyntaxTree::Tokens::reverse_iterator rit;
	CommandListOperatorToken *rbracket, *lbracket;
	std::shared_ptr<CommandListSyntaxTree> inner;

	for (i = tree->tokens.begin(); i != tree->tokens.end(); i++) {
		rbracket = dynamic_cast<CommandListOperatorToken*>(i->get());
		if (rbracket && !rbracket->token.compare(L")")) {
			for (rit = std::reverse_iterator<CommandListSyntaxTree::Tokens::iterator>(i); rit != tree->tokens.rend(); rit++) {
				lbracket = dynamic_cast<CommandListOperatorToken*>(rit->get());
				if (lbracket && !lbracket->token.compare(L"(")) {
					inner = std::make_shared<CommandListSyntaxTree>(lbracket->token_pos);
					// XXX: Double check bounds are right:
					inner->tokens.assign(rit.base(), i);
					i = tree->tokens.erase(rit.base() - 1, i + 1);
					i = tree->tokens.insert(i, std::move(inner));
					goto continue_rbracket_search; // continue would continue wrong loop
				}
			}
			throw CommandListSyntaxError(L"Unmatched )", rbracket->token_pos);
		}
	continue_rbracket_search: false;
	}

	for (i = tree->tokens.begin(); i != tree->tokens.end(); i++) {
		lbracket = dynamic_cast<CommandListOperatorToken*>(i->get());
		if (lbracket && !lbracket->token.compare(L"("))
			throw CommandListSyntaxError(L"Unmatched (", lbracket->token_pos);
	}
}

//template <class CommandListEqualityOperator>
static void convert_operators(CommandListSyntaxTree *tree)
{
	CommandListSyntaxTree::Tokens::iterator i;
	std::shared_ptr<CommandListOperatorToken> token;
	std::shared_ptr<CommandListEqualityOperator> op;
	std::shared_ptr<CommandListOperandBase> lhs;
	std::shared_ptr<CommandListOperandBase> rhs;

	if (CommandListEqualityOperator::right_associative())
		throw CommandListSyntaxError(L"FIXME: Implement right-associativity", 0);
	if (CommandListEqualityOperator::unary())
		throw CommandListSyntaxError(L"FIXME: Implement unary operators", 0);

	for (i = tree->tokens.begin() + 1; i < tree->tokens.end() - 1; i++) {
		token = dynamic_pointer_cast<CommandListOperatorToken>(*i);
		// TODO: Walk recursively down syntax trees and operators
		if (token && !token->token.compare(CommandListEqualityOperator::pattern())) {
			LogInfo("Operator matched\n");
			lhs = dynamic_pointer_cast<CommandListOperandBase>(*(i-1));
			if (!lhs) // FIXME: Drop this for unary-
				throw CommandListSyntaxError(L"Expected: Operand", lhs->token_pos);
			rhs = dynamic_pointer_cast<CommandListOperandBase>(*(i+1));
			if (!rhs) // FIXME: Drop this for unary-
				throw CommandListSyntaxError(L"Expected: Operand", rhs->token_pos);
			if (lhs && rhs) {
				op = std::make_shared<CommandListEqualityOperator>(std::move(lhs), *token, std::move(rhs));
				i = tree->tokens.erase(i - 1, i + 2);
				i = tree->tokens.insert(i, std::move(op));
			}
		}
	}
}

static void _log_syntax_tree(CommandListSyntaxTree *tree)
{
	CommandListSyntaxTree::Tokens::iterator i;
	CommandListSyntaxTree *inner;
	CommandListOperatorToken *op;
	CommandListOperand *operand;

	LogInfoNoNL("SyntaxTree[ ");
	for (i = tree->tokens.begin(); i != tree->tokens.end(); i++) {
		inner = dynamic_cast<CommandListSyntaxTree*>(i->get());
		op = dynamic_cast<CommandListOperatorToken*>(i->get());
		operand = dynamic_cast<CommandListOperand*>(i->get());
		if (inner) {
			_log_syntax_tree(inner);
		} else if (op) {
			LogInfoNoNL("Operator \"%S\"", (*i)->token.c_str());
		} else if (operand) {
			LogInfoNoNL("Operand \"%S\"", (*i)->token.c_str());
		} else {
			LogInfoNoNL("Token \"%S\"", (*i)->token.c_str());
		}
		if (i != tree->tokens.end()-1)
			LogInfoNoNL(", ");
	}
	LogInfoNoNL(" ]");
}

static void log_syntax_tree(CommandListSyntaxTree *tree)
{
	_log_syntax_tree(tree);
	LogInfo("\n");
}

bool CommandListExpression::parse(const wstring *expression, const wstring *ini_namespace, bool command_list_context)
{
	CommandListSyntaxTree tree(0);

	try {
		tokenise(expression, &tree, ini_namespace, command_list_context);
		LogInfo("After tokenisation, before parenthesis grouping:\n");
		log_syntax_tree(&tree);

		group_parenthesis(&tree);
		LogInfo("After parenthesis grouping:\n");
		log_syntax_tree(&tree);

		//convert_operators<CommandListEqualityOperator>(&tree);
		convert_operators(&tree);
		LogInfo("After processing == operators:\n");
		log_syntax_tree(&tree);

		evaluatable = tree.finalise();
		return true;
	} catch (const CommandListSyntaxError &e) {
		LogOverlay(LOG_WARNING_MONOSPACE,
				"Syntax Error: %S\n"
				"              %*s: %S\n",
				expression->c_str(), (int)e.pos+1, "^", e.msg.c_str());
		return false;
	}
}

float CommandListExpression::evaluate(CommandListState *state, HackerDevice *device)
{
	return evaluatable->evaluate(state, device);
}

bool CommandListExpression::static_evaluate(float *ret, HackerDevice *device)
{
	return evaluatable->static_evaluate(ret, device);
}

bool CommandListExpression::optimise(HackerDevice *device)
{
	if (!evaluatable) {
		LogOverlay(LOG_DIRE, "BUG: Non-evaluatable expression, please report this and provide your d3dx.ini\n");
		evaluatable = std::make_shared<CommandListOperand>(0, L"<BUG>");
		return false;
	}
	return evaluatable->optimise(device);
}

std::shared_ptr<CommandListEvaluatable> CommandListOperator::finalise()
{
	auto lhs_finalisable = dynamic_pointer_cast<CommandListFinalisable>(lhs_tree);
	auto rhs_finalisable = dynamic_pointer_cast<CommandListFinalisable>(rhs_tree);
	auto lhs_evaluatable = dynamic_pointer_cast<CommandListEvaluatable>(lhs_tree);
	auto rhs_evaluatable = dynamic_pointer_cast<CommandListEvaluatable>(rhs_tree);

	if (!lhs && lhs_finalisable)
		lhs = lhs_finalisable->finalise();
	if (!lhs && lhs_evaluatable)
		lhs = lhs_evaluatable;
	if (!lhs)
		throw CommandListSyntaxError(L"BUG: LHS operand invalid", token_pos);

	if (!rhs && rhs_finalisable)
		rhs = rhs_finalisable->finalise();
	if (!rhs && rhs_evaluatable)
		rhs = rhs_evaluatable;
	if (!rhs)
		throw CommandListSyntaxError(L"BUG: RHS operand invalid", token_pos);

	return nullptr;
}

std::shared_ptr<CommandListEvaluatable> CommandListSyntaxTree::finalise()
{
	std::shared_ptr<CommandListFinalisable> finalisable;
	std::shared_ptr<CommandListEvaluatable> evaluatable;
	std::shared_ptr<CommandListToken> token;
	Tokens::iterator i;

	for (i = tokens.begin(); i != tokens.end(); i++) {
		finalisable = dynamic_pointer_cast<CommandListFinalisable>(*i);
		if (finalisable) {
			evaluatable = finalisable->finalise();
			if (evaluatable) {
				// A recursive syntax tree has been finalised
				// and we replace it with its sole evaluatable
				// contents:
				token = dynamic_pointer_cast<CommandListToken>(evaluatable);
				if (!token) {
					LogInfo("BUG: finalised token did not cast back\n");
					DoubleBeepExit();
				}
				i = tokens.erase(i);
				i = tokens.insert(i, std::move(token));
			}
		}
	}

	// A finalised syntax tree should be reduced to a single evaluatable
	// operator/operand, which we pass back up the stack to replace this
	// tree
	if (tokens.empty())
		throw CommandListSyntaxError(L"Empty expression", 0);

	if (tokens.size() > 1)
		throw CommandListSyntaxError(L"Unexpected", tokens[1]->token_pos);

	evaluatable = dynamic_pointer_cast<CommandListEvaluatable>(tokens[0]);
	if (!evaluatable)
		throw CommandListSyntaxError(L"Non-evaluatable", tokens[0]->token_pos);

	return evaluatable;
}

float CommandListOperator::evaluate(CommandListState *state, HackerDevice *device)
{
	return evaluate(lhs->evaluate(state, device), rhs->evaluate(state, device));
}

bool CommandListOperator::static_evaluate(float *ret, HackerDevice *device)
{
	float lhs_static, rhs_static;

	if (lhs->static_evaluate(&lhs_static, device) && rhs->static_evaluate(&rhs_static, device)) {
		if (ret)
			*ret = evaluate(lhs_static, rhs_static);
		return true;
	}

	return false;
}

// TODO bool CommandListOperator::optimise(HackerDevice *device)
// TODO {
// TODO 	// FIXME: Stub
// TODO 	return false;
// TODO }

void ParamOverride::run(CommandListState *state)
{
	float *dest = &(G->iniParams[param_idx].*param_component);
	float orig = *dest;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	*dest = expression.evaluate(state);

	COMMAND_LIST_LOG(state, "  ini param override = %f\n", *dest);

	state->update_params |= (*dest != orig);
}

bool ParamOverride::optimise(HackerDevice *device)
{
	return expression.optimise(device);
}

static bool operand_allowed_in_context(ParamOverrideType type, bool command_list_context)
{
	if (command_list_context)
		return true;

	switch (type) {
		case ParamOverrideType::VALUE:
		case ParamOverrideType::INI_PARAM:
		case ParamOverrideType::RES_WIDTH:
		case ParamOverrideType::RES_HEIGHT:
		case ParamOverrideType::TIME:
		case ParamOverrideType::RAW_SEPARATION:
		case ParamOverrideType::CONVERGENCE:
		case ParamOverrideType::EYE_SEPARATION:
		case ParamOverrideType::STEREO_ACTIVE:
		case ParamOverrideType::SLI:
			return true;
	}
	return false;
}

bool CommandListOperand::parse(const wstring *operand, const wstring *ini_namespace, bool command_list_context)
{
	int ret, len1;

	// Try parsing value as a float
	ret = swscanf_s(operand->c_str(), L"%f%n", &val, &len1);
	if (ret != 0 && ret != EOF && len1 == operand->length()) {
		type = ParamOverrideType::VALUE;
		return operand_allowed_in_context(type, command_list_context);
	}

	// Try parsing operand as an ini param:
	if (ParseIniParamName(operand->c_str(), &param_idx, &param_component)) {
		type = ParamOverrideType::INI_PARAM;
		return operand_allowed_in_context(type, command_list_context);
	}

	// Try parsing value as a resource target for texture filtering
	ret = texture_filter_target.ParseTarget(operand->c_str(), true, ini_namespace);
	if (ret) {
		type = ParamOverrideType::TEXTURE;
		return operand_allowed_in_context(type, command_list_context);
	}

	// Try parsing value as a scissor rectangle. scissor_<side> also
	// appears in the keywords list for uses of the default rectangle 0.
	ret = swscanf_s(operand->c_str(), L"scissor%u_%n", &scissor, &len1);
	if (ret == 1 && scissor < D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
		if (!wcscmp(operand->c_str() + len1, L"left"))
			type = ParamOverrideType::SCISSOR_LEFT;
		else if (!wcscmp(operand->c_str() + len1, L"top"))
			type = ParamOverrideType::SCISSOR_TOP;
		else if (!wcscmp(operand->c_str() + len1, L"right"))
			type = ParamOverrideType::SCISSOR_RIGHT;
		else if (!wcscmp(operand->c_str() + len1, L"bottom"))
			type = ParamOverrideType::SCISSOR_BOTTOM;
		else
			return false;
		return operand_allowed_in_context(type, command_list_context);
	}

	// Check special keywords
	type = lookup_enum_val<const wchar_t *, ParamOverrideType>
		(ParamOverrideTypeNames, operand->c_str(), ParamOverrideType::INVALID);
	if (type != ParamOverrideType::INVALID)
		return operand_allowed_in_context(type, command_list_context);

	return false;
}

// Parse IniParams overrides, in forms such as
// x = 0.3 (set parameter to specific value, e.g. for shader partner filtering)
// y2 = ps-t0 (use parameter for texture filtering based on texture slot of shader type)
// z3 = rt_width / rt_height (set parameter to render target width/height)
// w4 = res_width / res_height (set parameter to resolution width/height)
bool ParseCommandListIniParamOverride(const wchar_t *section,
		const wchar_t *key, wstring *val, CommandList *command_list,
		const wstring *ini_namespace)
{
	ParamOverride *param = new ParamOverride();

	if (!ParseIniParamName(key, &param->param_idx, &param->param_component))
		goto bail;

	if (!param->expression.parse(val, ini_namespace))
		goto bail;

	param->ini_line = L"[" + wstring(section) + L"] " + wstring(key) + L" = " + *val;
	command_list->commands.push_back(std::shared_ptr<CommandListCommand>(param));
	return true;
bail:
	delete param;
	return false;
}

ResourcePool::~ResourcePool()
{
	unordered_map<uint32_t, ID3D11Resource*>::iterator i;

	for (i = cache.begin(); i != cache.end(); i++) {
		if (i->second)
			i->second->Release();
	}
	cache.clear();
}

void ResourcePool::emplace(uint32_t hash, ID3D11Resource *resource)
{
	if (resource)
		resource->AddRef();
	cache.emplace(hash, resource);
}

template <typename ResourceType,
	 typename DescType,
	HRESULT (__stdcall ID3D11Device::*CreateResource)(THIS_
	      const DescType *pDesc,
	      const D3D11_SUBRESOURCE_DATA *pInitialData,
	      ResourceType **ppTexture)
	>
static ResourceType* GetResourceFromPool(
		wstring *ini_line,
		ResourceType *src_resource,
		ResourceType *dst_resource,
		ResourcePool *resource_pool,
		CommandListState *state,
		DescType *desc)
{
	ResourceType *resource = NULL;
	DescType old_desc;
	uint32_t hash;
	size_t size;
	HRESULT hr;
	ResourcePoolCache::iterator pool_i;

	// We don't want to use the CalTexture2D/3DDescHash functions because
	// the resolution override could produce the same hash for distinct
	// texture descriptions. This hash isn't exposed to the user, so
	// doesn't matter what we use - just has to be fast.
	hash = crc32c_hw(0, desc, sizeof(DescType));

	pool_i = resource_pool->cache.find(hash);
	if (pool_i != resource_pool->cache.end()) {
		resource = (ResourceType*)pool_i->second;
		if (resource == dst_resource)
			return NULL;
		if (resource) {
			LogDebug("Switching cached resource %S\n", ini_line->c_str());
			resource->AddRef();
		}
	} else {
		LogInfo("Creating cached resource %S\n", ini_line->c_str());

		hr = (state->mOrigDevice1->*CreateResource)(desc, NULL, &resource);
		if (FAILED(hr)) {
			LogInfo("Resource copy failed %S: 0x%x\n", ini_line->c_str(), hr);
			LogResourceDesc(desc);
			src_resource->GetDesc(&old_desc);
			LogInfo("Original resource was:\n");
			LogResourceDesc(&old_desc);

			// Prevent further attempts:
			resource_pool->emplace(hash, NULL);

			return NULL;
		}
		resource_pool->emplace(hash, resource);
		size = resource_pool->cache.size();
		if (size > 1)
			LogInfo("  NOTICE: cache now contains %Ii resources\n", size);

		LogDebugResourceDesc(desc);
	}

	return resource;
}

CustomResource::CustomResource() :
	resource(NULL),
	view(NULL),
	is_null(true),
	substantiated(false),
	bind_flags((D3D11_BIND_FLAG)0),
	stride(0),
	offset(0),
	buf_size(0),
	format(DXGI_FORMAT_UNKNOWN),
	max_copies_per_frame(0),
	frame_no(0),
	copies_this_frame(0),
	override_type(CustomResourceType::INVALID),
	override_mode(CustomResourceMode::DEFAULT),
	override_bind_flags(CustomResourceBindFlags::INVALID),
	override_format((DXGI_FORMAT)-1),
	override_width(-1),
	override_height(-1),
	override_depth(-1),
	override_mips(-1),
	override_array(-1),
	override_msaa(-1),
	override_msaa_quality(-1),
	override_byte_width(-1),
	override_stride(-1),
	width_multiply(1.0f),
	height_multiply(1.0f),
	initial_data(NULL),
	initial_data_size(0)
{}

CustomResource::~CustomResource()
{
	if (resource)
		resource->Release();
	if (view)
		view->Release();
	free(initial_data);
}

bool CustomResource::OverrideSurfaceCreationMode(StereoHandle mStereoHandle, NVAPI_STEREO_SURFACECREATEMODE *orig_mode)
{

	if (override_mode == CustomResourceMode::DEFAULT)
		return false;

	NvAPI_Stereo_GetSurfaceCreationMode(mStereoHandle, orig_mode);

	switch (override_mode) {
		case CustomResourceMode::STEREO:
			NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,
					NVAPI_STEREO_SURFACECREATEMODE_FORCESTEREO);
			return true;
		case CustomResourceMode::MONO:
			NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,
					NVAPI_STEREO_SURFACECREATEMODE_FORCEMONO);
			return true;
		case CustomResourceMode::AUTO:
			NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,
					NVAPI_STEREO_SURFACECREATEMODE_AUTO);
			return true;
	}

	return false;
}

void CustomResource::Substantiate(ID3D11Device *mOrigDevice1, StereoHandle mStereoHandle)
{
	NVAPI_STEREO_SURFACECREATEMODE orig_mode = NVAPI_STEREO_SURFACECREATEMODE_AUTO;
	bool restore_create_mode = false;

	// We only allow a custom resource to be substantiated once. Otherwise
	// we could end up reloading it again if it is later set to null. Also
	// prevents us from endlessly retrying to load a custom resource from a
	// file that doesn't exist:
	if (substantiated)
		return;
	substantiated = true;

	// If this custom resource has already been set through other means we
	// won't overwrite it:
	if (resource || view)
		return;

	// If the resource section has enough information to create a resource
	// we do so the first time it is loaded from. The reason we do it this
	// late is to make sure we know which device is actually being used to
	// render the game - FC4 creates about a dozen devices with different
	// parameters while probing the hardware before it settles on the one
	// it will actually use.

	restore_create_mode = OverrideSurfaceCreationMode(mStereoHandle, &orig_mode);

	if (!filename.empty()) {
		LoadFromFile(mOrigDevice1);
	} else {
		switch (override_type) {
			case CustomResourceType::BUFFER:
			case CustomResourceType::STRUCTURED_BUFFER:
			case CustomResourceType::RAW_BUFFER:
				SubstantiateBuffer(mOrigDevice1, NULL, 0);
				break;
			case CustomResourceType::TEXTURE1D:
				SubstantiateTexture1D(mOrigDevice1);
				break;
			case CustomResourceType::TEXTURE2D:
			case CustomResourceType::CUBE:
				SubstantiateTexture2D(mOrigDevice1);
				break;
			case CustomResourceType::TEXTURE3D:
				SubstantiateTexture3D(mOrigDevice1);
				break;
		}
	}

	if (restore_create_mode)
		NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle, orig_mode);
}

void CustomResource::LoadBufferFromFile(ID3D11Device *mOrigDevice1)
{
	DWORD size, read_size;
	void *buf = NULL;
	HANDLE f;

	f = CreateFile(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		LogOverlay(LOG_WARNING, "Failed to load custom buffer resource %S: %d\n", filename.c_str(), GetLastError());
		return;
	}

	size = GetFileSize(f, 0);
	buf = malloc(size); // malloc to allow realloc to resize it if the user overrode the size
	if (!buf) {
		LogOverlay(LOG_DIRE, "Out of memory loading %S\n", filename.c_str());
		goto out_close;
	}

	if (!ReadFile(f, buf, size, &read_size, 0) || size != read_size) {
		LogOverlay(LOG_WARNING, "Error reading custom buffer from file %S\n", filename.c_str());
		goto out_delete;
	}

	SubstantiateBuffer(mOrigDevice1, &buf, size);

out_delete:
	free(buf);
out_close:
	CloseHandle(f);
}

void CustomResource::LoadFromFile(ID3D11Device *mOrigDevice1)
{
	wstring ext;
	HRESULT hr;

	switch (override_type) {
		case CustomResourceType::BUFFER:
		case CustomResourceType::STRUCTURED_BUFFER:
		case CustomResourceType::RAW_BUFFER:
			return LoadBufferFromFile(mOrigDevice1);
	}

	// This code path doesn't get a chance to override the resource
	// description, since DirectXTK takes care of that, but we can pass in
	// bind flags at least, which is sometimes necessary in complex
	// situations where 3DMigoto cannot automatically determine these or
	// when manipulating driver heuristics:
	if (override_bind_flags != CustomResourceBindFlags::INVALID)
		bind_flags = (D3D11_BIND_FLAG)override_bind_flags;

	// XXX: We are not creating a view with DirecXTK because
	// 1) it assumes we want a shader resource view, which is an
	//    assumption that doesn't fit with the goal of this code to
	//    allow for arbitrary resource copying, and
	// 2) we currently won't use the view in a source custom
	//    resource, even if we are referencing it into a compatible
	//    slot. We might improve this, and if we do, I don't want
	//    any surprises caused by a view of the wrong type we
	//    happen to have created here and forgotten about.
	// If we do start using the source custom resource's view, we
	// could do something smart here, like only using it if the
	// bind_flags indicate it will be used as a shader resource.

	ext = filename.substr(filename.rfind(L"."));
	if (!_wcsicmp(ext.c_str(), L".dds")) {
		LogInfoW(L"Loading custom resource %s as DDS\n", filename.c_str());
		hr = DirectX::CreateDDSTextureFromFileEx(mOrigDevice1,
				filename.c_str(), 0,
				D3D11_USAGE_DEFAULT, bind_flags, 0, 0,
				false, &resource, NULL, NULL);
	} else {
		LogInfoW(L"Loading custom resource %s as WIC\n", filename.c_str());
		hr = DirectX::CreateWICTextureFromFileEx(mOrigDevice1,
				filename.c_str(), 0,
				D3D11_USAGE_DEFAULT, bind_flags, 0, 0,
				false, &resource, NULL);
	}
	if (SUCCEEDED(hr)) {
		is_null = false;
		// TODO:
		// format = ...
	} else
		LogOverlay(LOG_WARNING, "Failed to load custom texture resource %S: 0x%x\n", filename.c_str(), hr);
}

void CustomResource::SubstantiateBuffer(ID3D11Device *mOrigDevice1, void **buf, DWORD size)
{
	D3D11_SUBRESOURCE_DATA data = {0}, *pInitialData = NULL;
	ID3D11Buffer *buffer;
	D3D11_BUFFER_DESC desc;
	HRESULT hr;

	if (!buf) {
		// If no file is passed in, we use the optional initial data to
		// initialise the buffer. We do this even if no initial data
		// has been specified, so that the buffer will be initialised
		// with zeroes for safety.
		buf = &initial_data;
		size = (DWORD)initial_data_size;
	}

	memset(&desc, 0, sizeof(desc));
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;

	// Allow the buffer size to be set from the file / initial data size,
	// but it can be overridden if specified explicitly. If it's a
	// structured buffer, we assume just a single structure by default, but
	// again this can be overridden. The reason for doing this here, and
	// not in OverrideBufferDesc, is that this only applies if we are
	// substantiating the resource from scratch, not when copying a resource.
	if (size) {
		desc.ByteWidth = size;
		if (override_type == CustomResourceType::STRUCTURED_BUFFER)
			desc.StructureByteStride = size;
	}

	OverrideBufferDesc(&desc);

	if (desc.ByteWidth > 0) {
		// Fill in size from the file/initial data, allowing for an
		// override to make it larger or smaller, which may involve
		// reallocating the buffer from the caller.
		if (desc.ByteWidth > size) {
			void *new_buf = realloc(*buf, desc.ByteWidth);
			if (!new_buf) {
				LogInfo("Out of memory enlarging buffer: [%S]\n", name.c_str());
				return;
			}
			memset((char*)new_buf + size, 0, desc.ByteWidth - size);
			*buf = new_buf;
		}

		data.pSysMem = *buf;
		pInitialData = &data;
	}

	hr = mOrigDevice1->CreateBuffer(&desc, pInitialData, &buffer);
	if (SUCCEEDED(hr)) {
		LogInfo("Substantiated custom %S [%S]\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str());
		LogDebugResourceDesc(&desc);
		resource = (ID3D11Resource*)buffer;
		is_null = false;
		OverrideOutOfBandInfo(&format, &stride);
	} else {
		LogOverlay(LOG_NOTICE, "Failed to substantiate custom %S [%S]: 0x%x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), hr);
		LogResourceDesc(&desc);
	}
}
void CustomResource::SubstantiateTexture1D(ID3D11Device *mOrigDevice1)
{
	ID3D11Texture1D *tex1d;
	D3D11_TEXTURE1D_DESC desc;
	HRESULT hr;

	memset(&desc, 0, sizeof(desc));
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;
	OverrideTexDesc(&desc);

	hr = mOrigDevice1->CreateTexture1D(&desc, NULL, &tex1d);
	if (SUCCEEDED(hr)) {
		LogInfo("Substantiated custom %S [%S]\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str());
		LogDebugResourceDesc(&desc);
		resource = (ID3D11Resource*)tex1d;
		is_null = false;
	} else {
		LogOverlay(LOG_NOTICE, "Failed to substantiate custom %S [%S]: 0x%x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), hr);
		LogResourceDesc(&desc);
	}
}
void CustomResource::SubstantiateTexture2D(ID3D11Device *mOrigDevice1)
{
	ID3D11Texture2D *tex2d;
	D3D11_TEXTURE2D_DESC desc;
	HRESULT hr;

	memset(&desc, 0, sizeof(desc));
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;
	OverrideTexDesc(&desc);

	hr = mOrigDevice1->CreateTexture2D(&desc, NULL, &tex2d);
	if (SUCCEEDED(hr)) {
		LogInfo("Substantiated custom %S [%S]\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str());
		LogDebugResourceDesc(&desc);
		resource = (ID3D11Resource*)tex2d;
		is_null = false;
	} else {
		LogOverlay(LOG_NOTICE, "Failed to substantiate custom %S [%S]: 0x%x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), hr);
		LogResourceDesc(&desc);
	}
}
void CustomResource::SubstantiateTexture3D(ID3D11Device *mOrigDevice1)
{
	ID3D11Texture3D *tex3d;
	D3D11_TEXTURE3D_DESC desc;
	HRESULT hr;

	memset(&desc, 0, sizeof(desc));
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;
	OverrideTexDesc(&desc);

	hr = mOrigDevice1->CreateTexture3D(&desc, NULL, &tex3d);
	if (SUCCEEDED(hr)) {
		LogInfo("Substantiated custom %S [%S]\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str());
		LogDebugResourceDesc(&desc);
		resource = (ID3D11Resource*)tex3d;
		is_null = false;
	} else {
		LogOverlay(LOG_NOTICE, "Failed to substantiate custom %S [%S]: 0x%x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), hr);
		LogResourceDesc(&desc);
	}
}

void CustomResource::OverrideBufferDesc(D3D11_BUFFER_DESC *desc)
{
	switch (override_type) {
		case CustomResourceType::STRUCTURED_BUFFER:
			desc->MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			break;
		case CustomResourceType::RAW_BUFFER:
			desc->MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			break;
	}

	if (override_stride != -1)
		desc->StructureByteStride = override_stride;
	else if (override_format != (DXGI_FORMAT)-1 && override_format != DXGI_FORMAT_UNKNOWN)
		desc->StructureByteStride = dxgi_format_size(override_format);

	if (override_byte_width != -1)
		desc->ByteWidth = override_byte_width;
	else if (override_array != -1)
		desc->ByteWidth = desc->StructureByteStride * override_array;

	if (override_bind_flags != CustomResourceBindFlags::INVALID)
		desc->BindFlags = (D3D11_BIND_FLAG)override_bind_flags;

	// TODO: Add more overrides for misc flags
}

void CustomResource::OverrideTexDesc(D3D11_TEXTURE1D_DESC *desc)
{
	if (override_width != -1)
		desc->Width = override_width;
	if (override_mips != -1)
		desc->MipLevels = override_mips;
	if (override_array != -1)
		desc->ArraySize = override_array;
	if (override_format != (DXGI_FORMAT)-1)
		desc->Format = override_format;

	desc->Width = (UINT)(desc->Width * width_multiply);

	if (override_bind_flags != CustomResourceBindFlags::INVALID)
		desc->BindFlags = (D3D11_BIND_FLAG)override_bind_flags;

	// TODO: Add more overrides for misc flags
}

void CustomResource::OverrideTexDesc(D3D11_TEXTURE2D_DESC *desc)
{
	if (override_width != -1)
		desc->Width = override_width;
	if (override_height != -1)
		desc->Height = override_height;
	if (override_mips != -1)
		desc->MipLevels = override_mips;
	if (override_format != (DXGI_FORMAT)-1)
		desc->Format = override_format;
	if (override_array != -1)
		desc->ArraySize = override_array;
	if (override_msaa != -1)
		desc->SampleDesc.Count = override_msaa;
	if (override_msaa_quality != -1)
		desc->SampleDesc.Quality = override_msaa_quality;

	if (override_type == CustomResourceType::CUBE) {
		desc->MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
		if (override_array != -1)
			desc->ArraySize = override_array * 6;
	}

	desc->Width = (UINT)(desc->Width * width_multiply);
	desc->Height = (UINT)(desc->Height * height_multiply);

	if (override_bind_flags != CustomResourceBindFlags::INVALID)
		desc->BindFlags = (D3D11_BIND_FLAG)override_bind_flags;

	// TODO: Add more overrides for misc flags
}

void CustomResource::OverrideTexDesc(D3D11_TEXTURE3D_DESC *desc)
{
	if (override_width != -1)
		desc->Width = override_width;
	if (override_height != -1)
		desc->Height = override_height;
	if (override_depth != -1)
		desc->Height = override_depth;
	if (override_mips != -1)
		desc->MipLevels = override_mips;
	if (override_format != (DXGI_FORMAT)-1)
		desc->Format = override_format;

	desc->Width = (UINT)(desc->Width * width_multiply);
	desc->Height = (UINT)(desc->Height * height_multiply);

	if (override_bind_flags != CustomResourceBindFlags::INVALID)
		desc->BindFlags = (D3D11_BIND_FLAG)override_bind_flags;

	// TODO: Add more overrides for misc flags
}

void CustomResource::OverrideOutOfBandInfo(DXGI_FORMAT *format, UINT *stride)
{
	if (override_format != (DXGI_FORMAT)-1)
		*format = override_format;
	if (override_stride != -1)
		*stride = override_stride;
}


bool ResourceCopyTarget::ParseTarget(const wchar_t *target,
		bool is_source, const wstring *ini_namespace)
{
	int ret, len;
	size_t length = wcslen(target);
	CustomResources::iterator res;

	ret = swscanf_s(target, L"%lcs-cb%u%n", &shader_type, 1, &slot, &len);
	if (ret == 2 && len == length && slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
		type = ResourceCopyTargetType::CONSTANT_BUFFER;
		goto check_shader_type;
	}

	ret = swscanf_s(target, L"%lcs-t%u%n", &shader_type, 1, &slot, &len);
	if (ret == 2 && len == length && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
		type = ResourceCopyTargetType::SHADER_RESOURCE;
	       goto check_shader_type;
	}

	// TODO: ret = swscanf_s(target, L"%lcs-s%u%n", &shader_type, 1, &slot, &len);
	// TODO: if (ret == 2 && len == length && slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
	// TODO: 	type = ResourceCopyTargetType::SAMPLER;
	// TODO:	goto check_shader_type;
	// TODO: }

	ret = swscanf_s(target, L"o%u%n", &slot, &len);
	if (ret == 1 && len == length && slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) {
		type = ResourceCopyTargetType::RENDER_TARGET;
		return true;
	}

	if (!wcscmp(target, L"od")) {
		type = ResourceCopyTargetType::DEPTH_STENCIL_TARGET;
		return true;
	}

	ret = swscanf_s(target, L"%lcs-u%u%n", &shader_type, 1, &slot, &len);
	// XXX: On Win8 D3D11_1_UAV_SLOT_COUNT (64) is the limit instead. Use
	// the lower amount for now to enforce compatibility.
	if (ret == 2 && len == length && slot < D3D11_PS_CS_UAV_REGISTER_COUNT) {
		// These views are only valid for pixel and compute shaders:
		if (shader_type == L'p' || shader_type == L'c') {
			type = ResourceCopyTargetType::UNORDERED_ACCESS_VIEW;
			return true;
		}
		return false;
	}

	ret = swscanf_s(target, L"vb%u%n", &slot, &len);
	if (ret == 1 && len == length && slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
		type = ResourceCopyTargetType::VERTEX_BUFFER;
		return true;
	}

	if (!wcscmp(target, L"ib")) {
		type = ResourceCopyTargetType::INDEX_BUFFER;
		return true;
	}

	ret = swscanf_s(target, L"so%u%n", &slot, &len);
	if (ret == 1 && len == length && slot < D3D11_SO_STREAM_COUNT) {
		type = ResourceCopyTargetType::STREAM_OUTPUT;
		return true;
	}

	if (is_source && !wcscmp(target, L"null")) {
		type = ResourceCopyTargetType::EMPTY;
		return true;
	}

	if (length >= 9 && !wcsncmp(target, L"resource", 8)) {
		// section name should already have been transformed to lower
		// case from ParseCommandList, so our keys will be consistent
		// in the unordered_map:
		wstring resource_id(target);
		wstring namespaced_section;

		res = customResources.end();
		if (get_namespaced_section_name_lower(&resource_id, ini_namespace, &namespaced_section))
			res = customResources.find(namespaced_section);
		if (res == customResources.end())
			res = customResources.find(resource_id);
		if (res == customResources.end())
			return false;

		custom_resource = &res->second;
		type = ResourceCopyTargetType::CUSTOM_RESOURCE;
		return true;
	}

	// Alternate means to assign StereoParams and IniParams
	if (is_source && !wcscmp(target, L"stereoparams")) {
		type = ResourceCopyTargetType::STEREO_PARAMS;
		return true;
	}

	if (is_source && !wcscmp(target, L"iniparams")) {
		type = ResourceCopyTargetType::INI_PARAMS;
		return true;
	}

	if (is_source && !wcscmp(target, L"cursor_mask")) {
		type = ResourceCopyTargetType::CURSOR_MASK;
		return true;
	}

	if (is_source && !wcscmp(target, L"cursor_color")) {
		type = ResourceCopyTargetType::CURSOR_COLOR;
		return true;
	}

	if (is_source && !wcscmp(target, L"this")) {
		type = ResourceCopyTargetType::THIS_RESOURCE;
		return true;
	}

	// XXX: Any reason to allow access to sequential swap chains? Given
	// they either won't exist or are read only I can't think of one.
	if (is_source && !wcscmp(target, L"bb")) { // Back Buffer
		type = ResourceCopyTargetType::SWAP_CHAIN;
		// Holding a reference on the back buffer will prevent
		// ResizeBuffers() from working, so forbid caching any views of
		// the back buffer. Leaving it bound could also be a problem,
		// but since this is usually only used from custom shader
		// sections they will take care of unbinding it automatically:
		forbid_view_cache = true;
		return true;
	}

	if (is_source && !wcscmp(target, L"r_bb")) {
		type = ResourceCopyTargetType::REAL_SWAP_CHAIN;
		// Holding a reference on the back buffer will prevent
		// ResizeBuffers() from working, so forbid caching any views of
		// the back buffer. Leaving it bound could also be a problem,
		// but since this is usually only used from custom shader
		// sections they will take care of unbinding it automatically:
		forbid_view_cache = true;
		return true;
	}

	if (is_source && !wcscmp(target, L"f_bb")) {
		type = ResourceCopyTargetType::FAKE_SWAP_CHAIN;
		// Holding a reference on the back buffer will prevent
		// ResizeBuffers() from working, so forbid caching any views of
		// the back buffer. Leaving it bound could also be a problem,
		// but since this is usually only used from custom shader
		// sections they will take care of unbinding it automatically:
		forbid_view_cache = true;
		return true;
	}

	return false;

check_shader_type:
	switch(shader_type) {
		case L'v': case L'h': case L'd': case L'g': case L'p': case L'c':
			return true;
	}
	return false;
}


bool ParseCommandListResourceCopyDirective(const wchar_t *section,
		const wchar_t *key, wstring *val, CommandList *command_list,
		const wstring *ini_namespace)
{
	ResourceCopyOperation *operation = new ResourceCopyOperation();
	wchar_t buf[MAX_PATH];
	wchar_t *src_ptr = NULL;

	if (!operation->dst.ParseTarget(key, false, ini_namespace))
		goto bail;

	// parse_enum_option_string replaces spaces with NULLs, so it can't
	// operate on the buffer in the wstring directly. I could potentially
	// change it to work without modifying the string, but for now it's
	// easier to just make a copy of the string:
	if (val->length() >= MAX_PATH)
		goto bail;
	wcsncpy_s(buf, val->c_str(), MAX_PATH);

	operation->options = parse_enum_option_string<wchar_t *, ResourceCopyOptions>
		(ResourceCopyOptionNames, buf, &src_ptr);

	if (!src_ptr)
		goto bail;

	if (!operation->src.ParseTarget(src_ptr, true, ini_namespace))
		goto bail;

	if (!(operation->options & ResourceCopyOptions::COPY_TYPE_MASK)) {
		// If the copy method was not speficied make a guess.
		// References aren't always safe (e.g. a resource can't be both
		// an input and an output), and a resource may not have been
		// created with the right usage flags, so we'll err on the side
		// of doing a full copy if we aren't fairly sure.
		//
		// If we're merely copying a resource from one shader to
		// another without changnig the usage (e.g. giving the vertex
		// shader access to a constant buffer or texture from the pixel
		// shader) a reference is probably safe (unless the game
		// reassigns it to a different usage later and doesn't know
		// that our reference is still bound somewhere), but it would
		// not be safe to give a vertex shader access to the depth
		// buffer of the output merger stage, for example.
		//
		// If we are copying a resource into a custom resource (e.g.
		// for use from another draw call), do a full copy by default
		// in case the game alters the original.
		//
		// If we are assigning a render target, do so by reference
		// since we probably want the result reflected in the resource
		// we assigned to it. Mostly this would already work due to the
		// custom resource rules, but adding this rule should make
		// assigning the back buffer to a render target work.
		if (operation->dst.type == ResourceCopyTargetType::CUSTOM_RESOURCE)
			operation->options |= ResourceCopyOptions::COPY;
		else if (operation->dst.type == ResourceCopyTargetType::RENDER_TARGET)
			operation->options |= ResourceCopyOptions::REFERENCE;
		else if (operation->src.type == ResourceCopyTargetType::CUSTOM_RESOURCE)
			operation->options |= ResourceCopyOptions::REFERENCE;
		else if (operation->src.type == operation->dst.type)
			operation->options |= ResourceCopyOptions::REFERENCE;
		else if (operation->dst.type == ResourceCopyTargetType::SHADER_RESOURCE
				&& (operation->src.type == ResourceCopyTargetType::STEREO_PARAMS
				|| operation->src.type == ResourceCopyTargetType::INI_PARAMS
				|| operation->src.type == ResourceCopyTargetType::CURSOR_MASK
				|| operation->src.type == ResourceCopyTargetType::CURSOR_COLOR))
			operation->options |= ResourceCopyOptions::REFERENCE;
		else
			operation->options |= ResourceCopyOptions::COPY;
	}

	// FIXME: If custom resources are copied to other custom resources by
	// reference that are in turn bound to the pipeline we may not
	// propagate all the bind flags correctly depending on the order
	// everything is parsed. We'd need to construct a dependency graph
	// to fix this, but it's not clear that this combination would really
	// be used in practice, so for now this will do.
	// FIXME: The constant buffer bind flag can't be combined with others
	if (operation->src.type == ResourceCopyTargetType::CUSTOM_RESOURCE &&
			(operation->options & ResourceCopyOptions::REFERENCE)) {
		// Fucking C++ making this line 3x longer than it should be:
		operation->src.custom_resource->bind_flags = (D3D11_BIND_FLAG)
			(operation->src.custom_resource->bind_flags | operation->dst.BindFlags());
	}

	operation->ini_line = L"[" + wstring(section) + L"] " + wstring(key) + L" = " + *val;
	command_list->commands.push_back(std::shared_ptr<CommandListCommand>(operation));
	return true;
bail:
	delete operation;
	return false;
}

static bool ParseIfCommand(const wchar_t *section, const wstring *line,
		CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace)
{
	IfCommand *operation = new IfCommand();
	wstring expression = line->substr(line->find_first_not_of(L" \t", 3));

	if (!operation->expression.parse(&expression, ini_namespace))
		goto bail;

	return AddCommandToList(operation, NULL, NULL, pre_command_list, post_command_list, section, line->c_str(), NULL);
bail:
	delete operation;
	return false;
}

static bool ParseElseCommand(const wchar_t *section,
		CommandList *pre_command_list, CommandList *post_command_list)
{
	return AddCommandToList(new ElsePlaceholder(), NULL, NULL, pre_command_list, post_command_list, section, L"else", NULL);
}

static bool _ParseEndIfCommand(const wchar_t *section,
		CommandList *command_list, bool post)
{
	CommandList::Commands::reverse_iterator rit;
	IfCommand *if_command;
	ElsePlaceholder *else_command = NULL;
	CommandList::Commands::iterator else_pos = command_list->commands.end();

	for (rit = command_list->commands.rbegin(); rit != command_list->commands.rend(); rit++) {
		else_command = dynamic_cast<ElsePlaceholder*>(rit->get());
		if (else_command) {
			// C++ gotcha: reverse_iterator::base() points to the *next* element
			else_pos = rit.base() - 1;
		}

		if_command = dynamic_cast<IfCommand*>(rit->get());
		if (if_command) {
			// Transfer the commands since the if command until the
			// endif into the if command's true/false lists
			if (post && !if_command->post_finalised) {
				// C++ gotcha: reverse_iterator::base() points to the *next* element
				if_command->true_commands_post->commands.assign(rit.base(), else_pos);
				if_command->true_commands_post->ini_section = if_command->ini_line;
				if (else_pos != command_list->commands.end()) {
					// Discard the else placeholder command:
					if_command->false_commands_post->commands.assign(else_pos + 1, command_list->commands.end());
					if_command->false_commands_post->ini_section = if_command->ini_line + L" <else>";
				}
				command_list->commands.erase(rit.base(), command_list->commands.end());
				if_command->post_finalised = true;
				return true;
			} else if (!post && !if_command->pre_finalised) {
				// C++ gotcha: reverse_iterator::base() points to the *next* element
				if_command->true_commands_pre->commands.assign(rit.base(), else_pos);
				if_command->true_commands_pre->ini_section = if_command->ini_line;
				if (else_pos != command_list->commands.end()) {
					// Discard the else placeholder command:
					if_command->false_commands_pre->commands.assign(else_pos + 1, command_list->commands.end());
					if_command->false_commands_pre->ini_section = if_command->ini_line + L" <else>";
				}
				command_list->commands.erase(rit.base(), command_list->commands.end());
				if_command->pre_finalised = true;
				return true;
			}
		}
	}

	return false;
}

static bool ParseEndIfCommand(const wchar_t *section,
		CommandList *pre_command_list, CommandList *post_command_list)
{
	bool ret;

	return _ParseEndIfCommand(section, pre_command_list, false)
	    && _ParseEndIfCommand(section, post_command_list, true);

	return ret;
}

bool ParseCommandListFlowControl(const wchar_t *section, const wstring *line,
		CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace)
{
	if (!wcsncmp(line->c_str(), L"if ", 3))
		return ParseIfCommand(section, line, pre_command_list, post_command_list, ini_namespace);
	// TODO if (!wcsncmp(line->c_str(), L"elif ", 5))
	// TODO	return ParseElseIfCommand(section, line->substr(5), pre_command_list, post_command_list, ini_namespace);
	if (!wcscmp(line->c_str(), L"else"))
		return ParseElseCommand(section, pre_command_list, post_command_list);
	if (!wcscmp(line->c_str(), L"endif"))
		return ParseEndIfCommand(section, pre_command_list, post_command_list);

	return false;
}

IfCommand::IfCommand() :
	pre_finalised(false),
	post_finalised(false)
{
	true_commands_pre = std::make_shared<CommandList>();
	true_commands_post = std::make_shared<CommandList>();
	false_commands_pre = std::make_shared<CommandList>();
	false_commands_post = std::make_shared<CommandList>();
	true_commands_post->post = true;
	false_commands_post->post = true;

	// Placeholder names to be replaced by endif processing - we should
	// never see these, but in case they do show up somewhere these will
	// provide a clue as to what they are:
	true_commands_pre->ini_section = L"if placeholder";
	true_commands_post->ini_section = L"if placeholder";
	false_commands_pre->ini_section = L"else placeholder";
	false_commands_post->ini_section = L"else placeholder";

	// Place the dynamically allocated command lists in this data structure
	// to ensure they stay alive until after the optimisation stage, even
	// if the IfCommand is freed, e.g. by being optimised out:
	dynamically_allocated_command_lists.push_back(true_commands_pre);
	dynamically_allocated_command_lists.push_back(true_commands_post);
	dynamically_allocated_command_lists.push_back(false_commands_pre);
	dynamically_allocated_command_lists.push_back(false_commands_post);

	// And register these command lists for later optimisation:
	registered_command_lists.push_back(true_commands_pre.get());
	registered_command_lists.push_back(true_commands_post.get());
	registered_command_lists.push_back(false_commands_pre.get());
	registered_command_lists.push_back(false_commands_post.get());
}

void IfCommand::run(CommandListState *state)
{
	if (expression.evaluate(state)) {
		COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());
		if (state->post)
			_RunCommandList(true_commands_post.get(), state);
		else
			_RunCommandList(true_commands_pre.get(), state);
	} else {
		COMMAND_LIST_LOG(state, "%S\n", else_line.c_str());
		if (state->post)
			_RunCommandList(false_commands_post.get(), state);
		else
			_RunCommandList(false_commands_pre.get(), state);
	}
}

bool IfCommand::optimise(HackerDevice *device)
{
	return expression.optimise(device);
}

bool IfCommand::noop(bool post, bool ignore_cto)
{
	float static_val;
	bool is_static;

	if ((post && !post_finalised) || (!post && !pre_finalised)) {
		LogOverlay(LOG_WARNING, "WARNING: If missing endif: %S\n", ini_line.c_str());
		return true;
	}

	is_static = expression.static_evaluate(&static_val);
	if (is_static) {
		if (static_val) {
			false_commands_pre->commands.clear();
			false_commands_post->commands.clear();
		} else {
			true_commands_pre->commands.clear();
			true_commands_post->commands.clear();
		}
	}

	if (post)
		return true_commands_post->commands.empty() && false_commands_post->commands.empty();
	return true_commands_pre->commands.empty() && false_commands_pre->commands.empty();
}

void CommandPlaceholder::run(CommandListState*)
{
	LogOverlay(LOG_DIRE, "BUG: Placeholder command executed: %S\n", ini_line.c_str());
}

bool CommandPlaceholder::noop(bool post, bool ignore_cto)
{
	LogOverlay(LOG_WARNING, "WARNING: Command not terminated: %S\n", ini_line.c_str());
	return true;
}

ID3D11Resource *ResourceCopyTarget::GetResource(
		CommandListState *state,
		ID3D11View **view,   // Used by textures, render targets, depth/stencil buffers & UAVs
		UINT *stride,        // Used by vertex buffers
		UINT *offset,        // Used by vertex & index buffers
		DXGI_FORMAT *format, // Used by index buffers
		UINT *buf_size)      // Used when creating a view of the buffer
{
	HackerDevice *mHackerDevice = state->mHackerDevice;
	ID3D11Device *mOrigDevice1 = state->mOrigDevice1;
	ID3D11DeviceContext *mOrigContext1 = state->mOrigContext1;
	ID3D11Resource *res = NULL;
	ID3D11Buffer *buf = NULL;
	ID3D11Buffer *so_bufs[D3D11_SO_STREAM_COUNT];
	ID3D11ShaderResourceView *resource_view = NULL;
	ID3D11RenderTargetView *render_view[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView *depth_view = NULL;
	ID3D11UnorderedAccessView *unordered_view = NULL;
	unsigned i;

	switch(type) {
	case ResourceCopyTargetType::CONSTANT_BUFFER:
		// FIXME: On win8 (or with evil update?), we should use
		// Get/SetConstantBuffers1 and copy the offset into the buffer as well
		switch(shader_type) {
		case L'v':
			mOrigContext1->VSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'h':
			mOrigContext1->HSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'd':
			mOrigContext1->DSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'g':
			mOrigContext1->GSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'p':
			mOrigContext1->PSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'c':
			mOrigContext1->CSGetConstantBuffers(slot, 1, &buf);
			return buf;
		default:
			// Should not happen
			return NULL;
		}
		break;

	case ResourceCopyTargetType::SHADER_RESOURCE:
		switch(shader_type) {
		case L'v':
			mOrigContext1->VSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'h':
			mOrigContext1->HSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'd':
			mOrigContext1->DSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'g':
			mOrigContext1->GSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'p':
			mOrigContext1->PSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'c':
			mOrigContext1->CSGetShaderResources(slot, 1, &resource_view);
			break;
		default:
			// Should not happen
			return NULL;
		}

		if (!resource_view)
			return NULL;

		resource_view->GetResource(&res);
		if (!res) {
			resource_view->Release();
			return NULL;
		}

		*view = resource_view;
		return res;

	// TODO: case ResourceCopyTargetType::SAMPLER: // Not an ID3D11Resource, need to think about this one
	// TODO: 	break;

	case ResourceCopyTargetType::VERTEX_BUFFER:
		// TODO: If copying this to a constant buffer, provide some
		// means to get the strides + offsets from within the shader.
		// Perhaps as an IniParam, or in another constant buffer?
		mOrigContext1->IAGetVertexBuffers(slot, 1, &buf, stride, offset);
		return buf;

	case ResourceCopyTargetType::INDEX_BUFFER:
		// TODO: Similar comment as vertex buffers above, provide a
		// means for a shader to get format + offset.
		mOrigContext1->IAGetIndexBuffer(&buf, format, offset);
		if (stride && format)
			*stride = dxgi_format_size(*format);
		return buf;

	case ResourceCopyTargetType::STREAM_OUTPUT:
		// XXX: Does not give us the offset
		mOrigContext1->SOGetTargets(slot + 1, so_bufs);

		// Release any buffers we aren't after:
		for (i = 0; i < slot; i++) {
			if (so_bufs[i]) {
				so_bufs[i]->Release();
				so_bufs[i] = NULL;
			}
		}

		return so_bufs[slot];

	case ResourceCopyTargetType::RENDER_TARGET:
		mOrigContext1->OMGetRenderTargets(slot + 1, render_view, NULL);

		// Release any views we aren't after:
		for (i = 0; i < slot; i++) {
			if (render_view[i]) {
				render_view[i]->Release();
				render_view[i] = NULL;
			}
		}

		if (!render_view[slot])
			return NULL;

		render_view[slot]->GetResource(&res);
		if (!res) {
			render_view[slot]->Release();
			return NULL;
		}

		*view = render_view[slot];
		return res;

	case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
		mOrigContext1->OMGetRenderTargets(0, NULL, &depth_view);
		if (!depth_view)
			return NULL;

		depth_view->GetResource(&res);
		if (!res) {
			depth_view->Release();
			return NULL;
		}

		// Depth buffers can't be buffers

		*view = depth_view;
		return res;

	case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
		switch(shader_type) {
		case L'p':
			// XXX: Not clear if the start slot is ok like this from the docs?
			// Particularly, what happens if we retrieve a subsequent UAV?
			mOrigContext1->OMGetRenderTargetsAndUnorderedAccessViews(0, NULL, NULL, slot, 1, &unordered_view);
			break;
		case L'c':
			mOrigContext1->CSGetUnorderedAccessViews(slot, 1, &unordered_view);
			break;
		default:
			// Should not happen
			return NULL;
		}

		if (!unordered_view)
			return NULL;

		unordered_view->GetResource(&res);
		if (!res) {
			unordered_view->Release();
			return NULL;
		}

		*view = unordered_view;
		return res;

	case ResourceCopyTargetType::CUSTOM_RESOURCE:
		custom_resource->Substantiate(mOrigDevice1, mHackerDevice->mStereoHandle);

		if (stride)
			*stride = custom_resource->stride;
		if (offset)
			*offset = custom_resource->offset;
		if (format)
			*format = custom_resource->format;
		if (buf_size)
			*buf_size = custom_resource->buf_size;

		if (custom_resource->is_null) {
			// Optimisation to allow the resource to be set to null
			// without throwing away the cache so we don't
			// endlessly create & destroy temporary resources.
			*view = NULL;
			return NULL;
		}

		if (custom_resource->view)
			custom_resource->view->AddRef();
		*view = custom_resource->view;
		if (custom_resource->resource)
			custom_resource->resource->AddRef();
		return custom_resource->resource;

	case ResourceCopyTargetType::STEREO_PARAMS:
		if (mHackerDevice->mStereoResourceView)
			mHackerDevice->mStereoResourceView->AddRef();
		*view = mHackerDevice->mStereoResourceView;
		if (mHackerDevice->mStereoTexture)
			mHackerDevice->mStereoTexture->AddRef();
		return mHackerDevice->mStereoTexture;

	case ResourceCopyTargetType::INI_PARAMS:
		if (mHackerDevice->mIniResourceView)
			mHackerDevice->mIniResourceView->AddRef();
		*view = mHackerDevice->mIniResourceView;
		if (mHackerDevice->mIniTexture)
			mHackerDevice->mIniTexture->AddRef();
		return mHackerDevice->mIniTexture;

	case ResourceCopyTargetType::CURSOR_MASK:
		UpdateCursorResources(state);
		if (state->cursor_mask_view)
			state->cursor_mask_view->AddRef();
		*view = state->cursor_mask_view;
		if (state->cursor_mask_tex)
			state->cursor_mask_tex->AddRef();
		return state->cursor_mask_tex;

	case ResourceCopyTargetType::CURSOR_COLOR:
		UpdateCursorResources(state);
		if (state->cursor_color_view)
			state->cursor_color_view->AddRef();
		*view = state->cursor_color_view;
		if (state->cursor_color_tex)
			state->cursor_color_tex->AddRef();
		return state->cursor_color_tex;

	case ResourceCopyTargetType::THIS_RESOURCE:
		if (state->view)
			state->view->AddRef();
		*view = state->view;
		if (state->resource)
			state->resource->AddRef();
		return state->resource;

	case ResourceCopyTargetType::SWAP_CHAIN:
		{
			HackerSwapChain *mHackerSwapChain = mHackerDevice->GetHackerSwapChain();
			if (mHackerSwapChain) {
				if (G->bb_is_upscaling_bb)
					mHackerSwapChain->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&res);
				else
					mHackerSwapChain->GetOrigSwapChain1()->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&res);
			} else
				COMMAND_LIST_LOG(state, "  Unable to get access to swap chain\n");
		}
		return res;

	case ResourceCopyTargetType::REAL_SWAP_CHAIN:
		{
			HackerSwapChain *mHackerSwapChain = mHackerDevice->GetHackerSwapChain();
			if (mHackerSwapChain)
				mHackerSwapChain->GetOrigSwapChain1()->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&res);
			else
				COMMAND_LIST_LOG(state, "  Unable to get access to real swap chain\n");
		}
		return res;

	case ResourceCopyTargetType::FAKE_SWAP_CHAIN:
		{
			HackerSwapChain *mHackerSwapChain = mHackerDevice->GetHackerSwapChain();
			if (mHackerSwapChain)
				mHackerSwapChain->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&res);
			else
				COMMAND_LIST_LOG(state, "  Unable to get access to fake swap chain\n");
		}
		return res;
	}

	return NULL;
}

void ResourceCopyTarget::SetResource(
		CommandListState *state,
		ID3D11Resource *res,
		ID3D11View *view,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT buf_size)
{
	ID3D11DeviceContext *mOrigContext1 = state->mOrigContext1;
	ID3D11Buffer *buf = NULL;
	ID3D11Buffer *so_bufs[D3D11_SO_STREAM_COUNT];
	ID3D11ShaderResourceView *resource_view = NULL;
	ID3D11RenderTargetView *render_view[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView *depth_view = NULL;
	ID3D11UnorderedAccessView *unordered_view = NULL;
	UINT uav_counter = -1; // TODO: Allow this to be set
	int i;

	switch(type) {
	case ResourceCopyTargetType::CONSTANT_BUFFER:
		// FIXME: On win8 (or with evil update?), we should use
		// Get/SetConstantBuffers1 and copy the offset into the buffer as well
		buf = (ID3D11Buffer*)res;
		switch(shader_type) {
		case L'v':
			mOrigContext1->VSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'h':
			mOrigContext1->HSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'd':
			mOrigContext1->DSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'g':
			mOrigContext1->GSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'p':
			mOrigContext1->PSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'c':
			mOrigContext1->CSSetConstantBuffers(slot, 1, &buf);
			return;
		default:
			// Should not happen
			return;
		}
		break;

	case ResourceCopyTargetType::SHADER_RESOURCE:
		resource_view = (ID3D11ShaderResourceView*)view;
		switch(shader_type) {
		case L'v':
			mOrigContext1->VSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'h':
			mOrigContext1->HSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'd':
			mOrigContext1->DSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'g':
			mOrigContext1->GSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'p':
			mOrigContext1->PSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'c':
			mOrigContext1->CSSetShaderResources(slot, 1, &resource_view);
			break;
		default:
			// Should not happen
			return;
		}
		break;

	// TODO: case ResourceCopyTargetType::SAMPLER: // Not an ID3D11Resource, need to think about this one
	// TODO: 	break;

	case ResourceCopyTargetType::VERTEX_BUFFER:
		buf = (ID3D11Buffer*)res;
		mOrigContext1->IASetVertexBuffers(slot, 1, &buf, &stride, &offset);
		return;

	case ResourceCopyTargetType::INDEX_BUFFER:
		buf = (ID3D11Buffer*)res;
		mOrigContext1->IASetIndexBuffer(buf, format, offset);
		break;

	case ResourceCopyTargetType::STREAM_OUTPUT:
		// XXX: HERE BE UNTESTED CODE PATHS!
		buf = (ID3D11Buffer*)res;
		mOrigContext1->SOGetTargets(D3D11_SO_STREAM_COUNT, so_bufs);
		if (so_bufs[slot])
			so_bufs[slot]->Release();
		so_bufs[slot] = buf;
		// XXX: We set offsets to NULL here. We should really preserve
		// them, but I'm not sure how to get their original values,
		// so... too bad. Probably will never even use this anyway.
		mOrigContext1->SOSetTargets(D3D11_SO_STREAM_COUNT, so_bufs, NULL);

		for (i = 0; i < D3D11_SO_STREAM_COUNT; i++) {
			if (so_bufs[i])
				so_bufs[i]->Release();
		}

		break;

	case ResourceCopyTargetType::RENDER_TARGET:
		mOrigContext1->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, render_view, &depth_view);

		if (render_view[slot])
			render_view[slot]->Release();
		render_view[slot] = (ID3D11RenderTargetView*)view;

		mOrigContext1->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, render_view, depth_view);

		for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
			if (i != slot && render_view[i])
				render_view[i]->Release();
		}
		if (depth_view)
			depth_view->Release();

		break;

	case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
		mOrigContext1->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, render_view, &depth_view);

		if (depth_view)
			depth_view->Release();
		depth_view = (ID3D11DepthStencilView*)view;

		mOrigContext1->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, render_view, depth_view);

		for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
			if (render_view[i])
				render_view[i]->Release();
		}
		break;

	case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
		// XXX: HERE BE UNTESTED CODE PATHS!
		unordered_view = (ID3D11UnorderedAccessView*)view;
		switch(shader_type) {
		case L'p':
			// XXX: Not clear if this will unbind other UAVs or not?
			// TODO: Allow pUAVInitialCounts to optionally be set
			mOrigContext1->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
				NULL, NULL, slot, 1, &unordered_view, &uav_counter);
			return;
		case L'c':
			// TODO: Allow pUAVInitialCounts to optionally be set
			mOrigContext1->CSSetUnorderedAccessViews(slot, 1, &unordered_view, &uav_counter);
			return;
		default:
			// Should not happen
			return;
		}
		break;

	case ResourceCopyTargetType::CUSTOM_RESOURCE:
		custom_resource->stride = stride;
		custom_resource->offset = offset;
		custom_resource->format = format;
		custom_resource->buf_size = buf_size;


		if (res == NULL && view == NULL) {
			// Optimisation to allow the resource to be set to null
			// without throwing away the cache so we don't
			// endlessly create & destroy temporary resources.
			custom_resource->is_null = true;
			return;
		}
		custom_resource->is_null = false;

		// If we are passed our own resource (might happen if the
		// resource is used directly in the run() function, or if
		// someone assigned a resource to itself), don't needlessly
		// AddRef() and Release(), and definitely don't Release()
		// before AddRef()
		if (custom_resource->view != view) {
			if (custom_resource->view)
				custom_resource->view->Release();
			custom_resource->view = view;
			if (custom_resource->view)
				custom_resource->view->AddRef();
		}

		if (custom_resource->resource != res) {
			if (custom_resource->resource)
				custom_resource->resource->Release();
			custom_resource->resource = res;
			if (custom_resource->resource)
				custom_resource->resource->AddRef();
		}
		break;

	case ResourceCopyTargetType::STEREO_PARAMS:
	case ResourceCopyTargetType::INI_PARAMS:
	case ResourceCopyTargetType::SWAP_CHAIN:
	case ResourceCopyTargetType::FAKE_SWAP_CHAIN:
	case ResourceCopyTargetType::CPU:
		// Only way we could "set" a resource to the (fake) back buffer is by
		// copying to it. Might implement overwrites later, but no
		// pressing need. To write something to the back buffer, assign
		// it as a render target instead.
		//
		// We can't set values on the CPU directly from here, since the
		// values won't have finished transferring yet. These will be
		// set from elsewhere.
		break;
	}
}

D3D11_BIND_FLAG ResourceCopyTarget::BindFlags()
{
	switch(type) {
		case ResourceCopyTargetType::CONSTANT_BUFFER:
			return D3D11_BIND_CONSTANT_BUFFER;
		case ResourceCopyTargetType::SHADER_RESOURCE:
			return D3D11_BIND_SHADER_RESOURCE;
		case ResourceCopyTargetType::VERTEX_BUFFER:
			return D3D11_BIND_VERTEX_BUFFER;
		case ResourceCopyTargetType::INDEX_BUFFER:
			return D3D11_BIND_INDEX_BUFFER;
		case ResourceCopyTargetType::STREAM_OUTPUT:
			return D3D11_BIND_STREAM_OUTPUT;
		case ResourceCopyTargetType::RENDER_TARGET:
			return D3D11_BIND_RENDER_TARGET;
		case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
			return D3D11_BIND_DEPTH_STENCIL;
		case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
			return D3D11_BIND_UNORDERED_ACCESS;
		case ResourceCopyTargetType::CUSTOM_RESOURCE:
			return custom_resource->bind_flags;
		case ResourceCopyTargetType::STEREO_PARAMS:
		case ResourceCopyTargetType::INI_PARAMS:
		case ResourceCopyTargetType::SWAP_CHAIN:
		case ResourceCopyTargetType::CPU:
			// N/A since swap chain can't be set as a destination
			return (D3D11_BIND_FLAG)0;
	}

	// Shouldn't happen. No return value makes sense, so raise an exception
	throw(std::range_error("Bad 3DMigoto ResourceCopyTarget"));
}

void ResourceCopyTarget::FindTextureOverrides(CommandListState *state, bool *resource_found, TextureOverrideMatches *matches)
{
	TextureOverrideMap::iterator i;
	ID3D11Resource *resource = NULL;
	ID3D11View *view = NULL;
	uint32_t hash = 0;

	resource = GetResource(state, &view, NULL, NULL, NULL, NULL);

	if (resource_found)
		*resource_found = !!resource;

	if (!resource)
		return;

	find_texture_overrides_for_resource(resource, matches, state->call_info);

	//COMMAND_LIST_LOG(state, "  found texture hash = %08llx\n", hash);

	resource->Release();
	if (view)
		view->Release();
}

static bool IsCoersionToStructuredBufferRequired(ID3D11View *view, UINT stride,
		UINT offset, DXGI_FORMAT format, D3D11_BIND_FLAG bind_flags)
{
	// If we are copying a vertex buffer into a shader resource we need to
	// convert it into a structured buffer, which requires a flag set when
	// creating the new resource as well as changes in the view.
	//
	// This function tries to detect this situation without explicitly
	// checking that the source was a vertex buffer - that way, similar
	// situations should work as well, such as when using an intermediate
	// resource.

	// If we are copying from a resource that had a view we will use it's
	// description to work out what we need to do (or we will, once I write
	// that code)
	if (view)
		return false;

	// If we know the format there's no need to be structured
	if (format != DXGI_FORMAT_UNKNOWN)
		return false;

	// We need to know the stride to be structured:
	if (stride == 0)
		return false;

	// Structured buffers only make sense for certain views:
	return !!(bind_flags & (D3D11_BIND_SHADER_RESOURCE |
			D3D11_BIND_RENDER_TARGET |
			D3D11_BIND_DEPTH_STENCIL |
			D3D11_BIND_UNORDERED_ACCESS));
}

static ID3D11Buffer *RecreateCompatibleBuffer(
		wstring *ini_line,
		ResourceCopyTarget *dst, // May be NULL
		ID3D11Buffer *src_resource,
		ID3D11Buffer *dst_resource,
		ResourcePool *resource_pool,
		ID3D11View *src_view,
		D3D11_BIND_FLAG bind_flags,
		CommandListState *state,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT *buf_dst_size)
{
	D3D11_BUFFER_DESC new_desc;
	ID3D11Buffer *buffer = NULL;
	UINT dst_size;

	src_resource->GetDesc(&new_desc);
	new_desc.BindFlags = bind_flags;

	if (dst && dst->type == ResourceCopyTargetType::CPU) {
		new_desc.Usage = D3D11_USAGE_STAGING;
		new_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	} else {
		new_desc.Usage = D3D11_USAGE_DEFAULT;
		new_desc.CPUAccessFlags = 0;
	}

	// TODO: Add a keyword to allow raw views:
	// D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS

	if (bind_flags & D3D11_BIND_CONSTANT_BUFFER) {
		// Constant buffers have additional limitations. The size must
		// be a multiple of 16, so round up if necessary, and it cannot
		// be larger than 4096 x 4 component x 4 byte constants.
		dst_size = (new_desc.ByteWidth + 15) & ~0xf;
		dst_size = min(dst_size, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16);

		// Constant buffers cannot be structured, so clear that flag:
		new_desc.MiscFlags &= ~D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		// XXX: Should we clear StructureByteStride? Seems to work ok
		// without clearing that.

		// If the size of the new resource doesn't match the old or
		// there is an offset we will have to perform a region copy
		// instead of a regular copy:
		if (offset || dst_size != new_desc.ByteWidth) {
			// It might be temping to take the offset into account
			// here and make the buffer only as large as it need to
			// be, but it's possible that the source offset might
			// change much more often than the source buffer (just
			// a guess), which could potentially lead us to
			// constantly recreating the destination buffer.

			// Note down the size of the source and destination:
			*buf_dst_size = dst_size;
			new_desc.ByteWidth = dst_size;
		}
	} else if (IsCoersionToStructuredBufferRequired(src_view, stride, offset, format, bind_flags)) {
		new_desc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		new_desc.StructureByteStride = stride;

		// A structured buffer needs to be a multiple of it's stride,
		// which may not be the case if we're converting a buffer to
		// one. Round it down:
		dst_size = new_desc.ByteWidth / stride * stride;
		// For now always using the region copy if there's an offset.
		// We might not need to do that if the offset is aligned to the
		// stride (although we would need to recreate the view every
		// time it changed), but for now it seems safest to use the
		// region copy method whenever there is an offset:
		if (offset || dst_size != new_desc.ByteWidth) {
			*buf_dst_size = dst_size;
			new_desc.ByteWidth = dst_size;
		}
	} else if (!src_view && offset) {
		// No source view but we do have an offset - use the region
		// copy to knock out the offset. We can probably assume the
		// original resource met all the size and alignment
		// constraints, so we shouldn't need to resize it.
		*buf_dst_size = new_desc.ByteWidth;
	}

	if (dst && dst->type == ResourceCopyTargetType::CUSTOM_RESOURCE)
		dst->custom_resource->OverrideBufferDesc(&new_desc);

	return GetResourceFromPool<ID3D11Buffer, D3D11_BUFFER_DESC, &ID3D11Device::CreateBuffer>
		(ini_line, src_resource, dst_resource, resource_pool, state, &new_desc);
}

static DXGI_FORMAT MakeTypeless(DXGI_FORMAT fmt)
{
	switch(fmt)
	{
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT:
			return DXGI_FORMAT_R32G32B32A32_TYPELESS;

		case DXGI_FORMAT_R32G32B32_FLOAT:
		case DXGI_FORMAT_R32G32B32_UINT:
		case DXGI_FORMAT_R32G32B32_SINT:
			return DXGI_FORMAT_R32G32B32_TYPELESS;

		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
			return DXGI_FORMAT_R16G16B16A16_TYPELESS;

		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G32_UINT:
		case DXGI_FORMAT_R32G32_SINT:
			return DXGI_FORMAT_R32G32_TYPELESS;

		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
		case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
			return DXGI_FORMAT_R32G8X24_TYPELESS;

		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R10G10B10A2_UINT:
			return DXGI_FORMAT_R10G10B10A2_TYPELESS;

		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R8G8B8A8_SINT:
			return DXGI_FORMAT_R8G8B8A8_TYPELESS;

		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_UINT:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_R16G16_SINT:
			return DXGI_FORMAT_R16G16_TYPELESS;

		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_R32_SINT:
			return DXGI_FORMAT_R32_TYPELESS;

		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
			return DXGI_FORMAT_R24G8_TYPELESS;

		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_UINT:
		case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R8G8_SINT:
			return DXGI_FORMAT_R8G8_TYPELESS;

		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R16_SINT:
			return DXGI_FORMAT_R16_TYPELESS;

		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_UINT:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_R8_SINT:
			return DXGI_FORMAT_R8_TYPELESS;

		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
			return DXGI_FORMAT_BC1_TYPELESS;

		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
			return DXGI_FORMAT_BC2_TYPELESS;

		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
			return DXGI_FORMAT_BC3_TYPELESS;

		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
			return DXGI_FORMAT_BC4_TYPELESS;

		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
			return DXGI_FORMAT_BC5_TYPELESS;

		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8A8_TYPELESS;

		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8X8_TYPELESS;

		case DXGI_FORMAT_BC6H_UF16:
		case DXGI_FORMAT_BC6H_SF16:
			return DXGI_FORMAT_BC6H_TYPELESS;

		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return DXGI_FORMAT_BC7_TYPELESS;

		case DXGI_FORMAT_R11G11B10_FLOAT:
		default:
			return fmt;
	}
}

static DXGI_FORMAT MakeDSVFormat(DXGI_FORMAT fmt)
{
	switch(fmt)
	{
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
		case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
			return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
			return DXGI_FORMAT_D32_FLOAT;

		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
			return DXGI_FORMAT_D24_UNORM_S8_UINT;

		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
			return DXGI_FORMAT_D16_UNORM;

		default:
			return EnsureNotTypeless(fmt);
	}
}

static DXGI_FORMAT MakeNonDSVFormat(DXGI_FORMAT fmt)
{
	// TODO: Add a keyword to return the stencil side of a combined
	// depth/stencil resource instead of the depth side
	switch(fmt)
	{
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
		case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
			return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;

		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
			return DXGI_FORMAT_R32_FLOAT;

		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
			return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
			return DXGI_FORMAT_R16_UNORM;

		default:
			return EnsureNotTypeless(fmt);
	}
}

// MSAA resolving only makes sense for Texture2D types, and the SampleDesc
// entry only exists in those. Use template specialisation so we don't have to
// duplicate the entire RecreateCompatibleTexture() routine for such a small
// difference.
template <typename DescType>
static void Texture2DDescResolveMSAA(DescType *desc) {}
template <>
static void Texture2DDescResolveMSAA(D3D11_TEXTURE2D_DESC *desc)
{
	desc->SampleDesc.Count = 1;
	desc->SampleDesc.Quality = 0;
}

template <typename ResourceType,
	 typename DescType,
	HRESULT (__stdcall ID3D11Device::*CreateTexture)(THIS_
	      const DescType *pDesc,
	      const D3D11_SUBRESOURCE_DATA *pInitialData,
	      ResourceType **ppTexture)
	>
static ResourceType* RecreateCompatibleTexture(
		wstring *ini_line,
		ResourceCopyTarget *dst, // May be NULL
		ResourceType *src_resource,
		ResourceType *dst_resource,
		ResourcePool *resource_pool,
		D3D11_BIND_FLAG bind_flags,
		CommandListState *state,
		StereoHandle mStereoHandle,
		ResourceCopyOptions options)
{
	DescType new_desc;

	src_resource->GetDesc(&new_desc);
	new_desc.BindFlags = bind_flags;

	if (dst && dst->type == ResourceCopyTargetType::CPU) {
		new_desc.Usage = D3D11_USAGE_STAGING;
		new_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	} else {
		new_desc.Usage = D3D11_USAGE_DEFAULT;
		new_desc.CPUAccessFlags = 0;
	}

	// New strategy - we make the new resources typeless whenever possible
	// and will fill the type back in in the view instead. This gives us
	// more flexibility with depth/stencil formats which need different
	// types depending on where they are bound in the pipeline. This also
	// helps with certain MSAA resources that may not be possible to create
	// if we change the type to a R*X* format.
	new_desc.Format = MakeTypeless(new_desc.Format);

	if (options & ResourceCopyOptions::STEREO2MONO)
		new_desc.Width *= 2;

	// TODO: reverse_blit might need to imply resolve_msaa:
	if (options & ResourceCopyOptions::RESOLVE_MSAA)
		Texture2DDescResolveMSAA(&new_desc);

	// XXX: Any changes needed in new_desc.MiscFlags?
	//
	// D3D11_RESOURCE_MISC_GENERATE_MIPS requires specific bind flags (both
	// shader resource AND render target must be set) and might prevent us
	// from creating the resource otherwise. Since we don't need to
	// generate mip-maps just clear it out:
	new_desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;

	if (dst && dst->type == ResourceCopyTargetType::CUSTOM_RESOURCE)
		dst->custom_resource->OverrideTexDesc(&new_desc);

	return GetResourceFromPool<ResourceType, DescType, CreateTexture>
		(ini_line, src_resource, dst_resource, resource_pool, state, &new_desc);
}

static void RecreateCompatibleResource(
		wstring *ini_line,
		ResourceCopyTarget *dst, // May be NULL
		ID3D11Resource *src_resource,
		ID3D11Resource **dst_resource,
		ResourcePool *resource_pool,
		ID3D11View *src_view,
		ID3D11View **dst_view,
		CommandListState *state,
		StereoHandle mStereoHandle,
		ResourceCopyOptions options,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT *buf_dst_size)
{
	NVAPI_STEREO_SURFACECREATEMODE orig_mode = NVAPI_STEREO_SURFACECREATEMODE_AUTO;
	D3D11_RESOURCE_DIMENSION src_dimension;
	D3D11_RESOURCE_DIMENSION dst_dimension;
	D3D11_BIND_FLAG bind_flags = (D3D11_BIND_FLAG)0;
	ID3D11Resource *res = NULL;
	bool restore_create_mode = false;

	if (dst)
		bind_flags = dst->BindFlags();

	src_resource->GetType(&src_dimension);
	if (*dst_resource) {
		(*dst_resource)->GetType(&dst_dimension);
		if (src_dimension != dst_dimension) {
			LogInfo("Resource type changed %S\n", ini_line->c_str());

			(*dst_resource)->Release();
			if (dst_view && *dst_view)
				(*dst_view)->Release();

			*dst_resource = NULL;
			if (dst_view)
				*dst_view = NULL;
		}
	}

	if (options & ResourceCopyOptions::CREATEMODE_MASK) {
		NvAPI_Stereo_GetSurfaceCreationMode(mStereoHandle, &orig_mode);
		restore_create_mode = true;

		// STEREO2MONO will force the final destination to mono since
		// it is in the CREATEMODE_MASK, but is not STEREO. It also
		// creates an additional intermediate resource that will be
		// forced to STEREO.

		if (options & ResourceCopyOptions::STEREO) {
			NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,
					NVAPI_STEREO_SURFACECREATEMODE_FORCESTEREO);
		} else {
			NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,
					NVAPI_STEREO_SURFACECREATEMODE_FORCEMONO);
		}
	} else if (dst && dst->type == ResourceCopyTargetType::CUSTOM_RESOURCE) {
		restore_create_mode = dst->custom_resource->OverrideSurfaceCreationMode(mStereoHandle, &orig_mode);
	}

	switch (src_dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			res = RecreateCompatibleBuffer(ini_line, dst, (ID3D11Buffer*)src_resource, (ID3D11Buffer*)*dst_resource,
				resource_pool, src_view, bind_flags, state, stride, offset, format, buf_dst_size);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			res = RecreateCompatibleTexture<ID3D11Texture1D, D3D11_TEXTURE1D_DESC, &ID3D11Device::CreateTexture1D>
				(ini_line, dst, (ID3D11Texture1D*)src_resource, (ID3D11Texture1D*)*dst_resource, resource_pool,
				 bind_flags, state, mStereoHandle, options);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			res = RecreateCompatibleTexture<ID3D11Texture2D, D3D11_TEXTURE2D_DESC, &ID3D11Device::CreateTexture2D>
				(ini_line, dst, (ID3D11Texture2D*)src_resource, (ID3D11Texture2D*)*dst_resource, resource_pool,
				 bind_flags, state, mStereoHandle, options);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			res = RecreateCompatibleTexture<ID3D11Texture3D, D3D11_TEXTURE3D_DESC, &ID3D11Device::CreateTexture3D>
				(ini_line, dst, (ID3D11Texture3D*)src_resource, (ID3D11Texture3D*)*dst_resource, resource_pool,
				 bind_flags, state, mStereoHandle, options);
			break;
	}

	if (restore_create_mode)
		NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle, orig_mode);

	if (res) {
		if (*dst_resource)
			(*dst_resource)->Release();
		if (dst_view && *dst_view)
			(*dst_view)->Release();

		*dst_resource = res;
		if (dst_view)
			*dst_view = NULL;
	}
}

template <typename DescType>
static void FillOutBufferDescCommon(DescType *desc, UINT stride,
		UINT offset, UINT buf_src_size)
{
	// The documentation on the buffer part of the description is
	// misleading.
	//
	// There are two unions with two possible parameters each which
	// are documented in MSDN, but DX11 never uses ElementWidth
	// (which is determined by either the format, or buffer's
	// StructureByteStride), only NumElements.
	//
	// My reading of FirstElement/ElementOffset sound like they are
	// the same thing, but one is in bytes and the other is in
	// elements - only the names seem backwards compared to the
	// description in the documentation. Research suggests DX11
	// only uses multiples of the element size (since it's a union,
	// it shouldn't matter which name we use).
	//
	// XXX: At the moment we are relying on the region copy to have
	// knocked out the offset for us. We could alternatively do it
	// here (and the below should work), but we would need to
	// create a new view every time the offset changes.
	//
	// TODO: Handle vertex/index buffers with "first vertex/index" here, or
	// give shaders a way to access that via ini params.
	if (stride) {
		desc->Buffer.FirstElement = offset / stride;
		desc->Buffer.NumElements = (buf_src_size - offset) / stride;
	} else {
		desc->Buffer.FirstElement = 0;
		desc->Buffer.NumElements = 1;
	}
}

static D3D11_SHADER_RESOURCE_VIEW_DESC* FillOutBufferDesc(
		D3D11_SHADER_RESOURCE_VIEW_DESC *desc, UINT stride,
		UINT offset, UINT buf_src_size)
{
	// TODO: Also handle BUFFEREX for raw buffers
	desc->ViewDimension = D3D11_SRV_DIMENSION_BUFFER;

	FillOutBufferDescCommon<D3D11_SHADER_RESOURCE_VIEW_DESC>(desc, stride, offset, buf_src_size);
	return desc;
}
static D3D11_RENDER_TARGET_VIEW_DESC* FillOutBufferDesc(
		D3D11_RENDER_TARGET_VIEW_DESC *desc, UINT stride,
		UINT offset, UINT buf_src_size)
{
	desc->ViewDimension = D3D11_RTV_DIMENSION_BUFFER;

	FillOutBufferDescCommon<D3D11_RENDER_TARGET_VIEW_DESC>(desc, stride, offset, buf_src_size);
	return desc;
}
static D3D11_UNORDERED_ACCESS_VIEW_DESC* FillOutBufferDesc(
		D3D11_UNORDERED_ACCESS_VIEW_DESC *desc, UINT stride,
		UINT offset, UINT buf_src_size)
{
	desc->ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	// TODO Support buffer UAV flags for append, counter and raw buffers.
	desc->Buffer.Flags = 0;

	FillOutBufferDescCommon<D3D11_UNORDERED_ACCESS_VIEW_DESC>(desc, stride, offset, buf_src_size);
	return desc;
}
static D3D11_DEPTH_STENCIL_VIEW_DESC* FillOutBufferDesc(
		D3D11_DEPTH_STENCIL_VIEW_DESC *desc, UINT stride,
		UINT offset, UINT buf_src_size)
{
	// Depth views don't support buffers:
	return NULL;
}


// This is a hell of a lot of duplicated code, mostly thanks to DX using
// different names for the same thing in a slightly different type, and pretty
// much all this is only needed for depth/stencil format conversions. It would
// be nice to refactor this somehow. TODO: For now we are creating a view of
// the entire resource, but it would make sense to use information from the
// source view if available instead.
static D3D11_SHADER_RESOURCE_VIEW_DESC* FillOutTex1DDesc(
		D3D11_SHADER_RESOURCE_VIEW_DESC *view_desc,
		D3D11_TEXTURE1D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->ArraySize == 1) {
		view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
		view_desc->Texture1D.MostDetailedMip = 0;
		view_desc->Texture1D.MipLevels = -1;
	} else {
		view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
		view_desc->Texture1DArray.MostDetailedMip = 0;
		view_desc->Texture1DArray.MipLevels = -1;
		view_desc->Texture1DArray.FirstArraySlice = 0;
		view_desc->Texture1DArray.ArraySize = resource_desc->ArraySize;
	}

	return view_desc;
}
static D3D11_RENDER_TARGET_VIEW_DESC* FillOutTex1DDesc(
		D3D11_RENDER_TARGET_VIEW_DESC *view_desc,
		D3D11_TEXTURE1D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->ArraySize == 1) {
		view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
		view_desc->Texture1D.MipSlice = 0;
	} else {
		view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
		view_desc->Texture1DArray.MipSlice = 0;
		view_desc->Texture1DArray.FirstArraySlice = 0;
		view_desc->Texture1DArray.ArraySize = resource_desc->ArraySize;
	}

	return view_desc;
}
static D3D11_DEPTH_STENCIL_VIEW_DESC* FillOutTex1DDesc(
		D3D11_DEPTH_STENCIL_VIEW_DESC *view_desc,
		D3D11_TEXTURE1D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeDSVFormat(format);

	if (resource_desc->ArraySize == 1) {
		view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
		view_desc->Texture1D.MipSlice = 0;
	} else {
		view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
		view_desc->Texture1DArray.MipSlice = 0;
		view_desc->Texture1DArray.FirstArraySlice = 0;
		view_desc->Texture1DArray.ArraySize = resource_desc->ArraySize;
	}

	return view_desc;
}
static D3D11_UNORDERED_ACCESS_VIEW_DESC* FillOutTex1DDesc(
		D3D11_UNORDERED_ACCESS_VIEW_DESC *view_desc,
		D3D11_TEXTURE1D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->ArraySize == 1) {
		view_desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
		view_desc->Texture1D.MipSlice = 0;
	} else {
		view_desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
		view_desc->Texture1DArray.MipSlice = 0;
		view_desc->Texture1DArray.FirstArraySlice = 0;
		view_desc->Texture1DArray.ArraySize = resource_desc->ArraySize;
	}

	return view_desc;
}
static D3D11_SHADER_RESOURCE_VIEW_DESC* FillOutTex2DDesc(
		D3D11_SHADER_RESOURCE_VIEW_DESC *view_desc,
		D3D11_TEXTURE2D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
			view_desc->TextureCube.MostDetailedMip = 0;
			view_desc->TextureCube.MipLevels = -1;
		} else {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
			view_desc->TextureCubeArray.MostDetailedMip = 0;
			view_desc->TextureCubeArray.MipLevels = -1;
			view_desc->TextureCubeArray.First2DArrayFace = 0; // FIXME: Get from original view
			view_desc->TextureCubeArray.NumCubes = resource_desc->ArraySize / 6;
		}
	} else if (resource_desc->SampleDesc.Count == 1) {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			view_desc->Texture2D.MostDetailedMip = 0;
			view_desc->Texture2D.MipLevels = -1;
		} else {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			view_desc->Texture2DArray.MostDetailedMip = 0;
			view_desc->Texture2DArray.MipLevels = -1;
			view_desc->Texture2DArray.FirstArraySlice = 0;
			view_desc->Texture2DArray.ArraySize = resource_desc->ArraySize;
		}
	} else {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
		} else {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
			view_desc->Texture2DMSArray.FirstArraySlice = 0;
			view_desc->Texture2DMSArray.ArraySize = resource_desc->ArraySize;
		}
	}

	return view_desc;
}
static D3D11_RENDER_TARGET_VIEW_DESC* FillOutTex2DDesc(
		D3D11_RENDER_TARGET_VIEW_DESC *view_desc,
		D3D11_TEXTURE2D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->SampleDesc.Count == 1) {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			view_desc->Texture2D.MipSlice = 0;
		} else {
			view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			view_desc->Texture2DArray.MipSlice = 0;
			view_desc->Texture2DArray.FirstArraySlice = 0;
			view_desc->Texture2DArray.ArraySize = resource_desc->ArraySize;
		}
	} else {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
		} else {
			view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
			view_desc->Texture2DMSArray.FirstArraySlice = 0;
			view_desc->Texture2DMSArray.ArraySize = resource_desc->ArraySize;
		}
	}

	return view_desc;
}
static D3D11_DEPTH_STENCIL_VIEW_DESC* FillOutTex2DDesc(
		D3D11_DEPTH_STENCIL_VIEW_DESC *view_desc,
		D3D11_TEXTURE2D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeDSVFormat(format);
	view_desc->Flags = 0; // TODO: Fill in from old view, and add keyword to override

	if (resource_desc->SampleDesc.Count == 1) {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			view_desc->Texture2D.MipSlice = 0;
		} else {
			view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			view_desc->Texture2DArray.MipSlice = 0;
			view_desc->Texture2DArray.FirstArraySlice = 0;
			view_desc->Texture2DArray.ArraySize = resource_desc->ArraySize;
		}
	} else {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
		} else {
			view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
			view_desc->Texture2DMSArray.FirstArraySlice = 0;
			view_desc->Texture2DMSArray.ArraySize = resource_desc->ArraySize;
		}
	}

	return view_desc;
}
static D3D11_UNORDERED_ACCESS_VIEW_DESC* FillOutTex2DDesc(
		D3D11_UNORDERED_ACCESS_VIEW_DESC *view_desc,
		D3D11_TEXTURE2D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->ArraySize == 1) {
		view_desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		view_desc->Texture2D.MipSlice = 0;
	} else {
		view_desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		view_desc->Texture2DArray.MipSlice = 0;
		view_desc->Texture2DArray.FirstArraySlice = 0;
		view_desc->Texture2DArray.ArraySize = resource_desc->ArraySize;
	}

	return view_desc;
}
static D3D11_SHADER_RESOURCE_VIEW_DESC* FillOutTex3DDesc(
		D3D11_SHADER_RESOURCE_VIEW_DESC *view_desc,
		D3D11_TEXTURE3D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
	view_desc->Texture3D.MostDetailedMip = 0;
	view_desc->Texture3D.MipLevels = -1;

	return view_desc;
}
static D3D11_RENDER_TARGET_VIEW_DESC* FillOutTex3DDesc(
		D3D11_RENDER_TARGET_VIEW_DESC *view_desc,
		D3D11_TEXTURE3D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
	view_desc->Texture3D.MipSlice = 0;
	view_desc->Texture3D.FirstWSlice = 0;
	view_desc->Texture3D.WSize = -1;

	return view_desc;
}
static D3D11_DEPTH_STENCIL_VIEW_DESC* FillOutTex3DDesc(
		D3D11_DEPTH_STENCIL_VIEW_DESC *view_desc,
		D3D11_TEXTURE3D_DESC *resource_desc, DXGI_FORMAT format)
{
	// DSV cannot be a Texture3D

	return NULL;
}
static D3D11_UNORDERED_ACCESS_VIEW_DESC* FillOutTex3DDesc(
		D3D11_UNORDERED_ACCESS_VIEW_DESC *view_desc,
		D3D11_TEXTURE3D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	view_desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
	view_desc->Texture3D.MipSlice = 0;
	view_desc->Texture3D.FirstWSlice = 0;
	view_desc->Texture3D.WSize = -1;

	return view_desc;
}


template <typename ViewType,
	 typename DescType,
	 HRESULT (__stdcall ID3D11Device::*CreateView)(THIS_
			 ID3D11Resource *pResource,
			 const DescType *pDesc,
			 ViewType **ppView)
	>
static ID3D11View* _CreateCompatibleView(
		ID3D11Resource *resource,
		CommandListState *state,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT buf_src_size)
{
	D3D11_RESOURCE_DIMENSION dimension;
	ID3D11Texture1D *tex1d;
	ID3D11Texture2D *tex2d;
	ID3D11Texture3D *tex3d;
	D3D11_TEXTURE1D_DESC tex1d_desc;
	D3D11_TEXTURE2D_DESC tex2d_desc;
	D3D11_TEXTURE3D_DESC tex3d_desc;
	ViewType *view = NULL;
	DescType view_desc, *pDesc = NULL;
	HRESULT hr;

	resource->GetType(&dimension);
	switch(dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			// In the case of a buffer type resource we must specify the
			// description as DirectX doesn't have enough information from the
			// buffer alone to create a view.

			view_desc.Format = format;

			pDesc = FillOutBufferDesc(&view_desc, stride, offset, buf_src_size);

			// This should already handle things like:
			// - Copying a vertex buffer to a SRV or constant buffer
			// - Copying an index buffer to a SRV
			// - Copying structured buffers
			// - Copying regular buffers

			// TODO: Support UAV flags like append/consume and SRV BufferEx views
			break;

		// We now also fill out the view description for textures as
		// well. We used to create fully typed resources and leave this
		// up to DX, but there were some situations where that would
		// not work (depth buffers need different types depending on
		// where they are bound, some MSAA resources could not be
		// created), so we now create typeless resources and therefore
		// have to fill out the view description to set the type. We
		// could potentially do this for only the cases where we need
		// (i.e. depth buffer formats), but I want to do this for
		// everything because it's so damn overly complex that typos
		// are ensured so this way it will at least get more exposure
		// and I can find the bugs sooner:
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			tex1d = (ID3D11Texture1D*)resource;
			tex1d->GetDesc(&tex1d_desc);
			pDesc = FillOutTex1DDesc(&view_desc, &tex1d_desc, format);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			tex2d = (ID3D11Texture2D*)resource;
			tex2d->GetDesc(&tex2d_desc);
			pDesc = FillOutTex2DDesc(&view_desc, &tex2d_desc, format);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			tex3d = (ID3D11Texture3D*)resource;
			tex3d->GetDesc(&tex3d_desc);
			pDesc = FillOutTex3DDesc(&view_desc, &tex3d_desc, format);
			break;
	}

	hr = (state->mOrigDevice1->*CreateView)(resource, pDesc, &view);
	if (FAILED(hr)) {
		LogInfo("Resource copy CreateCompatibleView failed: %x\n", hr);
		if (pDesc)
			LogViewDesc(pDesc);
		LogResourceDesc(resource);
		return NULL;
	}

	if (pDesc)
		LogDebugViewDesc(pDesc);

	return view;
}

static ID3D11View* CreateCompatibleView(
		ResourceCopyTarget *dst,
		ID3D11Resource *resource,
		CommandListState *state,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT buf_src_size)
{
	switch (dst->type) {
		case ResourceCopyTargetType::SHADER_RESOURCE:
			return _CreateCompatibleView<ID3D11ShaderResourceView,
			       D3D11_SHADER_RESOURCE_VIEW_DESC,
			       &ID3D11Device::CreateShaderResourceView>
				       (resource, state, stride, offset, format, buf_src_size);
		case ResourceCopyTargetType::RENDER_TARGET:
			return _CreateCompatibleView<ID3D11RenderTargetView,
			       D3D11_RENDER_TARGET_VIEW_DESC,
			       &ID3D11Device::CreateRenderTargetView>
				       (resource, state, stride, offset, format, buf_src_size);
		case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
			return _CreateCompatibleView<ID3D11DepthStencilView,
			       D3D11_DEPTH_STENCIL_VIEW_DESC,
			       &ID3D11Device::CreateDepthStencilView>
				       (resource, state, stride, offset, format, buf_src_size);
		case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
			return _CreateCompatibleView<ID3D11UnorderedAccessView,
			       D3D11_UNORDERED_ACCESS_VIEW_DESC,
			       &ID3D11Device::CreateUnorderedAccessView>
				       (resource, state, stride, offset, format, buf_src_size);
	}
	return NULL;
}

static void SetViewportFromResource(CommandListState *state, ID3D11Resource *resource)
{
	D3D11_RESOURCE_DIMENSION dimension;
	ID3D11Texture1D *tex1d;
	ID3D11Texture2D *tex2d;
	ID3D11Texture3D *tex3d;
	D3D11_TEXTURE1D_DESC tex1d_desc;
	D3D11_TEXTURE2D_DESC tex2d_desc;
	D3D11_TEXTURE3D_DESC tex3d_desc;
	D3D11_VIEWPORT viewport = {0, 0, 0, 0, D3D11_MIN_DEPTH, D3D11_MAX_DEPTH};

	// TODO: Could handle mip-maps from a view like the CD3D11_VIEWPORT
	// constructor, but we aren't using them elsewhere so don't care yet.
	resource->GetType(&dimension);
	switch(dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			// TODO: Width = NumElements
			return;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			tex1d = (ID3D11Texture1D*)resource;
			tex1d->GetDesc(&tex1d_desc);
			viewport.Width = (float)tex1d_desc.Width;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			tex2d = (ID3D11Texture2D*)resource;
			tex2d->GetDesc(&tex2d_desc);
			viewport.Width = (float)tex2d_desc.Width;
			viewport.Height = (float)tex2d_desc.Height;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			tex3d = (ID3D11Texture3D*)resource;
			tex3d->GetDesc(&tex3d_desc);
			viewport.Width = (float)tex3d_desc.Width;
			viewport.Height = (float)tex3d_desc.Height;
	}

	state->mOrigContext1->RSSetViewports(1, &viewport);
}

ResourceCopyOperation::ResourceCopyOperation() :
	options(ResourceCopyOptions::INVALID),
	cached_resource(NULL),
	cached_view(NULL),
	stereo2mono_intermediate(NULL)
{}

ResourceCopyOperation::~ResourceCopyOperation()
{
	if (cached_resource)
		cached_resource->Release();

	if (cached_view)
		cached_view->Release();
}

ResourceStagingOperation::ResourceStagingOperation()
{
	dst.type = ResourceCopyTargetType::CPU;
	options = ResourceCopyOptions::COPY;
	staging = false;
	ini_line = L"  Beginning transfer to CPU...";
}

HRESULT ResourceStagingOperation::map(CommandListState *state, D3D11_MAPPED_SUBRESOURCE *mapping)
{
	if (!cached_resource)
		return E_FAIL;

	return state->mOrigContext1->Map(cached_resource, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, mapping);
}

void ResourceStagingOperation::unmap(CommandListState *state)
{
	if (cached_resource)
		state->mOrigContext1->Unmap(cached_resource, 0);
}

static void ResolveMSAA(ID3D11Resource *dst_resource, ID3D11Resource *src_resource, CommandListState *state)
{
	UINT item, level, index, support;
	D3D11_RESOURCE_DIMENSION dst_dimension;
	ID3D11Texture2D *src, *dst;
	D3D11_TEXTURE2D_DESC desc;
	DXGI_FORMAT fmt;
	HRESULT hr;

	dst_resource->GetType(&dst_dimension);
	if (dst_dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
		return;

	src = (ID3D11Texture2D*)src_resource;
	dst = (ID3D11Texture2D*)dst_resource;

	dst->GetDesc(&desc);
	fmt = EnsureNotTypeless(desc.Format);

	hr = state->mOrigDevice1->CheckFormatSupport( fmt, &support );
	if (FAILED(hr) || !(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)) {
		// TODO: Implement a fallback using a SM5 shader to resolve it
		LogInfo("Resource copy cannot resolve MSAA format %d\n", fmt);
		return;
	}

	for (item = 0; item < desc.ArraySize; item++) {
		for (level = 0; level < desc.MipLevels; level++) {
			index = D3D11CalcSubresource(level, item, max(desc.MipLevels, 1));
			state->mOrigContext1->ResolveSubresource(dst, index, src, index, fmt);
		}
	}
}

static void ReverseStereoBlit(ID3D11Resource *dst_resource, ID3D11Resource *src_resource, CommandListState *state)
{
	NvAPI_Status nvret;
	D3D11_RESOURCE_DIMENSION src_dimension;
	ID3D11Texture2D *src;
	D3D11_TEXTURE2D_DESC srcDesc;
	UINT item, level, index, width, height;
	D3D11_BOX srcBox;
	int fallbackside, fallback = 0;

	src_resource->GetType(&src_dimension);
	if (src_dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		// TODO: I think it should be possible to do this with all
		// resource types (possibly including buffers from the
		// discovery of the stereo parameters in the cb12 slot), but I
		// need to test it and make sure it works first
		LogInfo("Resource copy: Reverse stereo blit not supported on resource type %d\n", src_dimension);
		return;
	}

	src = (ID3D11Texture2D*)src_resource;
	src->GetDesc(&srcDesc);

	// TODO: Resolve MSAA
	// TODO: Use intermediate resource if copying from a texture with depth buffer bind flags

	// If stereo is disabled the reverse stereo blit won't work and we
	// would end up with the destination only updated on the left, which
	// may lead to shaders reading stale or 0 data if they read from the
	// right hand side. Use the fallback path to copy the source to both
	// sides of the destination so that the right side will be up to date:
	fallback = state->mHackerDevice->mParamTextureManager.mActive ? 0 : 1;

	if (!fallback) {
		nvret = NvAPI_Stereo_ReverseStereoBlitControl(state->mHackerDevice->mStereoHandle, true);
		if (nvret != NVAPI_OK) {
			LogInfo("Resource copying failed to enable reverse stereo blit\n");
			// Fallback path: Copy 2D resource to both sides of the 2x
			// width destination
			fallback = 1;
		}
	}

	for (fallbackside = 0; fallbackside < 1 + fallback; fallbackside++) {

		// Set the source box as per the nvapi documentation:
		srcBox.left = 0;
		srcBox.top = 0;
		srcBox.front = 0;
		srcBox.right = width = srcDesc.Width;
		srcBox.bottom = height = srcDesc.Height;
		srcBox.back = 1;

		// Perform the reverse stereo blit on all sub-resources and mip-maps:
		for (item = 0; item < srcDesc.ArraySize; item++) {
			for (level = 0; level < srcDesc.MipLevels; level++) {
				index = D3D11CalcSubresource(level, item, max(srcDesc.MipLevels, 1));
				srcBox.right = width >> level;
				srcBox.bottom = height >> level;
				state->mOrigContext1->CopySubresourceRegion(dst_resource, index,
						fallbackside * srcBox.right, 0, 0,
						src, index, &srcBox);
			}
		}
	}

	if (!fallback)
		NvAPI_Stereo_ReverseStereoBlitControl(state->mHackerDevice->mStereoHandle, false);
}

static void SpecialCopyBufferRegion(ID3D11Resource *dst_resource,ID3D11Resource *src_resource,
		CommandListState *state, UINT stride, UINT *offset,
		UINT buf_src_size, UINT buf_dst_size)
{
	// We are copying a buffer for use in a constant buffer and the size of
	// the original buffer did not meet the constraints of a constant
	// buffer.
	D3D11_BOX src_box;

	// We want to copy from the offset to the end of the source buffer, but
	// cap it to the destination size to avoid "undefined behaviour". Keep
	// in mind that this is "right", not "size":
	src_box.left = *offset;
	src_box.right = min(buf_src_size, *offset + buf_dst_size);

	if (stride) {
		// If we are copying to a structured resource, the source box
		// must be a multiple of the stride, so round it down:
		src_box.right = (src_box.right - src_box.left) / stride * stride + src_box.left;
	}

	src_box.top = 0;
	src_box.bottom = 1;
	src_box.front = 0;
	src_box.back = 1;

	state->mOrigContext1->CopySubresourceRegion(dst_resource, 0, 0, 0, 0, src_resource, 0, &src_box);

	// We have effectively removed the offset during the region copy, so
	// set it to 0 to make sure nothing will try to use it again elsewhere:
	*offset = 0;
}

static UINT get_resource_bind_flags(ID3D11Resource *resource)
{
	D3D11_RESOURCE_DIMENSION dimension;
	ID3D11Buffer *buf = NULL;
	ID3D11Texture1D *tex1d = NULL;
	ID3D11Texture2D *tex2d = NULL;
	ID3D11Texture3D *tex3d = NULL;
	D3D11_BUFFER_DESC buf_desc;
	D3D11_TEXTURE1D_DESC tex1d_desc;
	D3D11_TEXTURE2D_DESC tex2d_desc;
	D3D11_TEXTURE3D_DESC tex3d_desc;

	resource->GetType(&dimension);
	switch (dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			buf = (ID3D11Buffer*)resource;
			buf->GetDesc(&buf_desc);
			return buf_desc.BindFlags;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			tex1d = (ID3D11Texture1D*)resource;
			tex1d->GetDesc(&tex1d_desc);
			return tex1d_desc.BindFlags;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			tex2d = (ID3D11Texture2D*)resource;
			tex2d->GetDesc(&tex2d_desc);
			return tex2d_desc.BindFlags;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			tex3d = (ID3D11Texture3D*)resource;
			tex3d->GetDesc(&tex3d_desc);
			return tex3d_desc.BindFlags;
	}
	return 0;
}

ID3D11View* ClearViewCommand::create_best_view(
		ID3D11Resource *resource,
		CommandListState *state,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT buf_src_size)
{
	UINT bind_flags;

	// We didn't get a view, so we will have to create one, but
	// which type? We will guess based on what the user specified
	// and what bind flags the resource has.

	FillInMissingInfo(target.type, resource, NULL, &stride, &offset,
			&buf_src_size, &format);

	// If the user specified "depth" and/or "stencil" they gave us
	// the answer:
	if (clear_depth || clear_stencil) {
		return _CreateCompatibleView<ID3D11DepthStencilView,
		       D3D11_DEPTH_STENCIL_VIEW_DESC,
		       &ID3D11Device::CreateDepthStencilView>
			       (resource, state, stride, offset, format, buf_src_size);
	}

	// If the user specified "int" or used a hex string then it
	// must be a UAV and we must be doing an int clear on it:
	if (clear_uav_uint) {
		return _CreateCompatibleView<ID3D11UnorderedAccessView,
		       D3D11_UNORDERED_ACCESS_VIEW_DESC,
		       &ID3D11Device::CreateUnorderedAccessView>
			       (resource, state, stride, offset, format, buf_src_size);
	}

	// Otherwise just make whatever view is compatible with the bind flags.
	// Since views may have multiple bind flags let's prioritise the more
	// esoteric DSV and UAV before RTV on the theory that if they are
	// available then we are more likely to want to use their clear
	// methods.
	bind_flags = get_resource_bind_flags(resource);
	if (bind_flags & D3D11_BIND_DEPTH_STENCIL) {
		return _CreateCompatibleView<ID3D11DepthStencilView,
		       D3D11_DEPTH_STENCIL_VIEW_DESC,
		       &ID3D11Device::CreateDepthStencilView>
			       (resource, state, stride, offset, format, buf_src_size);
	}
	if (bind_flags & D3D11_BIND_UNORDERED_ACCESS) {
		return _CreateCompatibleView<ID3D11UnorderedAccessView,
		       D3D11_UNORDERED_ACCESS_VIEW_DESC,
		       &ID3D11Device::CreateUnorderedAccessView>
			       (resource, state, stride, offset, format, buf_src_size);
	}
	if (bind_flags & D3D11_BIND_RENDER_TARGET) {
		return _CreateCompatibleView<ID3D11RenderTargetView,
		       D3D11_RENDER_TARGET_VIEW_DESC,
		       &ID3D11Device::CreateRenderTargetView>
			       (resource, state, stride, offset, format, buf_src_size);
	}
	// TODO: In DX 11.1 there is a generic clear routine, so SRVs might work?
	return NULL;
}

void ClearViewCommand::clear_unknown_view(ID3D11View *view, CommandListState *state)
{
	ID3D11RenderTargetView *rtv = NULL;
	ID3D11DepthStencilView *dsv = NULL;
	ID3D11UnorderedAccessView *uav = NULL;

	// We have a view, but we don't know what kind of view it is. We could
	// infer that from the target type, but in the future CustomResource
	// targets will return a cached view as well (they already have a view
	// today, but if you follow the logic closely you will realise we don't
	// have any code paths that will decide to use it), so to try to future
	// proof this let's use QueryInterface() to see which interfaces the
	// view supports to tell us what kind it is:
	view->QueryInterface(__uuidof(ID3D11RenderTargetView), (void**)&rtv);
	view->QueryInterface(__uuidof(ID3D11DepthStencilView), (void**)&dsv);
	view->QueryInterface(__uuidof(ID3D11UnorderedAccessView), (void**)&uav);

	if (rtv) {
		COMMAND_LIST_LOG(state, "  clearing RTV\n");
		state->mOrigContext1->ClearRenderTargetView(rtv, fval);
	}
	if (dsv) {
		D3D11_CLEAR_FLAG flags = (D3D11_CLEAR_FLAG)0;
		COMMAND_LIST_LOG(state, "  clearing DSV\n");

		if (!clear_depth && !clear_stencil)
			flags = (D3D11_CLEAR_FLAG)(D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL);
		else if (clear_depth)
			flags = D3D11_CLEAR_DEPTH;
		else if (clear_stencil)
			flags = D3D11_CLEAR_STENCIL;

		state->mOrigContext1->ClearDepthStencilView(dsv, flags, dsv_depth, dsv_stencil);
	}
	if (uav) {
		// We can clear UAVs with either floats or uints, but which
		// should we use? The API call doesn't let us know if it
		// failed, and floats will only work with specific view
		// formats, so we try to predict if the float clear will pass
		// unless the user specificially told us to use the int clear.
		if (clear_uav_uint || !UAVSupportsFloatClear(uav)) {
			COMMAND_LIST_LOG(state, "  clearing UAV (uint)\n");
			state->mOrigContext1->ClearUnorderedAccessViewUint(uav, uval);
		} else {
			COMMAND_LIST_LOG(state, "  clearing UAV (float)\n");
			state->mOrigContext1->ClearUnorderedAccessViewFloat(uav, fval);
		}
	}

	if (rtv)
		rtv->Release();
	if (dsv)
		dsv->Release();
	if (uav)
		uav->Release();
}

void ClearViewCommand::run(CommandListState *state)
{
	ID3D11Resource *resource = NULL;
	ID3D11View *view = NULL;
	UINT stride = 0;
	UINT offset = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT buf_src_size = 0;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	resource = target.GetResource(state, &view, &stride, &offset, &format, &buf_src_size);
	if (!resource) {
		COMMAND_LIST_LOG(state, "  No resource to clear\n");
		return;
	}

	if (!view)
		view = create_best_view(resource, state, stride, offset, format, buf_src_size);

	if (view)
		clear_unknown_view(view, state);
	else
		COMMAND_LIST_LOG(state, "  No view and unable to create view to clear resource\n");

	if (resource)
		resource->Release();
	if (view)
		view->Release();
}


static bool ViewMatchesResource(ID3D11View *view, ID3D11Resource *resource)
{
	ID3D11Resource *tmp_resource = NULL;

	view->GetResource(&tmp_resource);
	if (!tmp_resource)
		return false;
	tmp_resource->Release();

	return (tmp_resource == resource);
}

// Returns the equivelent target type of built in targets with pre-existing
// views, so that we don't go and create a view cache when we already have one
// we could use directly:
static ResourceCopyTargetType EquivTarget(ResourceCopyTargetType type)
{
	switch(type) {
		case ResourceCopyTargetType::STEREO_PARAMS:
		case ResourceCopyTargetType::INI_PARAMS:
		case ResourceCopyTargetType::CURSOR_MASK:
		case ResourceCopyTargetType::CURSOR_COLOR:
			return ResourceCopyTargetType::SHADER_RESOURCE;
	}
	return type;
}

void ResourceCopyOperation::run(CommandListState *state)
{
	HackerDevice *mHackerDevice = state->mHackerDevice;
	HackerContext *mHackerContext = state->mHackerContext;
	ID3D11DeviceContext *mOrigContext1 = state->mOrigContext1;
	ID3D11Resource *src_resource = NULL;
	ID3D11Resource *dst_resource = NULL;
	ID3D11Resource **pp_cached_resource = &cached_resource;
	ResourcePool *p_resource_pool = &resource_pool;
	ID3D11View *src_view = NULL;
	ID3D11View *dst_view = NULL;
	ID3D11View **pp_cached_view = &cached_view;
	UINT stride = 0;
	UINT offset = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT buf_src_size = 0, buf_dst_size = 0;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (src.type == ResourceCopyTargetType::EMPTY) {
		dst.SetResource(state, NULL, NULL, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
		return;
	}

	src_resource = src.GetResource(state, &src_view, &stride, &offset, &format, &buf_src_size);
	if (!src_resource) {
		COMMAND_LIST_LOG(state, "  Copy source was NULL\n");
		if (!(options & ResourceCopyOptions::UNLESS_NULL)) {
			// Still set destination to NULL - if we are copying a
			// resource we generally expect it to be there, and
			// this will make errors more obvious if we copy
			// something that doesn't exist. This behaviour can be
			// overridden with the unless_null keyword.
			dst.SetResource(state, NULL, NULL, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
		}
		return;
	}

	if (dst.type == ResourceCopyTargetType::CUSTOM_RESOURCE) {
		// If we're copying to a custom resource, use the resource &
		// view in the CustomResource directly as the cache instead of
		// the cache in the ResourceCopyOperation. This will reduce the
		// number of extra resources we have floating around if copying
		// something to a single custom resource from multiple shaders.
		pp_cached_resource = &dst.custom_resource->resource;
		p_resource_pool = &dst.custom_resource->resource_pool;
		pp_cached_view = &dst.custom_resource->view;

		if (dst.custom_resource->max_copies_per_frame) {
			if (dst.custom_resource->frame_no != G->frame_no) {
				dst.custom_resource->frame_no = G->frame_no;
				dst.custom_resource->copies_this_frame = 1;
			} else if (dst.custom_resource->copies_this_frame++ >= dst.custom_resource->max_copies_per_frame) {
				COMMAND_LIST_LOG(state, "  max_copies_per_frame exceeded\n");
				return;
			}
		}

		dst.custom_resource->OverrideOutOfBandInfo(&format, &stride);
	}

	FillInMissingInfo(src.type, src_resource, src_view, &stride, &offset, &buf_src_size, &format);

	if (options & ResourceCopyOptions::COPY_MASK) {
		RecreateCompatibleResource(&ini_line, &dst, src_resource,
			pp_cached_resource, p_resource_pool, src_view, pp_cached_view,
			state, mHackerDevice->mStereoHandle,
			options, stride, offset, format, &buf_dst_size);

		if (!*pp_cached_resource) {
			LogDebug("Resource copy error: Could not create/update destination resource\n");
			goto out_release;
		}
		dst_resource = *pp_cached_resource;
		dst_view = *pp_cached_view;

		if (options & ResourceCopyOptions::COPY_DESC) {
			// RecreateCompatibleResource has already done the work
			COMMAND_LIST_LOG(state, "  copying resource description\n");
		} else if (options & ResourceCopyOptions::STEREO2MONO) {
			COMMAND_LIST_LOG(state, "  performing reverse stereo blit\n");

			// TODO: Resolve MSAA to an intermediate resource first
			// if necessary (but keep in mind this may have
			// compatibility issues without a fallback path)

			// The reverse stereo blit seems to only work if the
			// destination resource is stereo. This is a bit
			// bizzare since the whole point of it is to create a
			// double width mono resource, but there you go.
			// We use a second intermediate resource that is forced
			// to stereo and the final destination is forced to
			// mono - once we have done the reverse blit we use an
			// ordinary copy to the final mono resource.

			RecreateCompatibleResource(&(ini_line + L" (intermediate)"),
				NULL, src_resource, &stereo2mono_intermediate,
				p_resource_pool, NULL, NULL,
				state, mHackerDevice->mStereoHandle,
				(ResourceCopyOptions)(options | ResourceCopyOptions::STEREO),
				stride, offset, format, NULL);

			ReverseStereoBlit(stereo2mono_intermediate, src_resource, state);

			mOrigContext1->CopyResource(dst_resource, stereo2mono_intermediate);

		} else if (options & ResourceCopyOptions::RESOLVE_MSAA) {
			COMMAND_LIST_LOG(state, "  resolving MSAA\n");
			ResolveMSAA(dst_resource, src_resource, state);
		} else if (buf_dst_size) {
			COMMAND_LIST_LOG(state, "  performing region copy\n");
			SpecialCopyBufferRegion(dst_resource, src_resource,
					state, stride, &offset,
					buf_src_size, buf_dst_size);
		} else {
			COMMAND_LIST_LOG(state, "  performing full copy\n");
			mOrigContext1->CopyResource(dst_resource, src_resource);
		}
	} else {
		COMMAND_LIST_LOG(state, "  copying by reference\n");
		dst_resource = src_resource;
		if (src_view && (EquivTarget(src.type) == EquivTarget(dst.type))) {
			dst_view = src_view;
		} else if (*pp_cached_view) {
			if (ViewMatchesResource(*pp_cached_view, dst_resource)) {
				dst_view = *pp_cached_view;
			} else {
				LogDebug("Resource copying: Releasing stale view cache\n");
				(*pp_cached_view)->Release();
				*pp_cached_view = NULL;
			}
		}
		// TODO: If we are referencing to/from a custom resource we
		// currently don't reference the view, but we could so long as
		// the bind flags from the original source are compatible with
		// the bind flags in the final destination. If we implement
		// this, go read the note in CustomResource::Substantiate()
	}

	if (!dst_view) {
		dst_view = CreateCompatibleView(&dst, dst_resource, state,
				stride, offset, format, buf_src_size);
		// Not checking for NULL return as view's are not applicable to
		// all types. Legitimate failures are logged.
		*pp_cached_view = dst_view;
	}

	dst.SetResource(state, dst_resource, dst_view, stride, offset, format, buf_dst_size);

	if (options & ResourceCopyOptions::SET_VIEWPORT)
		SetViewportFromResource(state, dst_resource);

out_release:

	if ((options & ResourceCopyOptions::NO_VIEW_CACHE || src.forbid_view_cache)
			&& *pp_cached_view)
	{
		(*pp_cached_view)->Release();
		*pp_cached_view = NULL;
	}

	if (src_view)
		src_view->Release();

	if (src_resource)
		src_resource->Release();
}
