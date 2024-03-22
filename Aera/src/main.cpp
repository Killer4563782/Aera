#include "pch/pch.h"
#include "memory/pointers.h"
#include "renderer/renderer.h"
#include "hooking/hooking.h"
#include "rage/commands/invoker/invoker.h"
#include "fiber/manager.h"
#include "fiber/pool.h"
#include "script/script.h"
#include "commands/commands.h"
#include "exceptions/handler.h"
#include "core/logger.h"
#include "fiber/dxfiber.h"
#include "gui/gui.h"
#include "thread pool/threadpool.h"
#include "util/imageLoaderHelpers.h"

std::unique_ptr<hooking> hooking_instance{};
std::unique_ptr<renderer> renderer_instance{};
std::unique_ptr<thread_pool> thread_pool_instance{};

std::mutex exit_mutex;

void thread_function()
{
	std::lock_guard lock(exit_mutex);
	std::exit(0);
}


inline bool disable_anti_cheat_skeleton()
{
	bool patched = false;
	for (rage::game_skeleton_update_mode* mode = pointers::g_game_skeleton->m_update_modes; mode; mode = mode->m_next)
	{
		for (rage::game_skeleton_update_base* update_node = mode->m_head; update_node; update_node = update_node->
		     m_next)
		{
			if (update_node->m_hash != rage::constexprJoaat("Common Main"))
				continue;

			auto group = reinterpret_cast<rage::game_skeleton_update_group*>(update_node);
			for (rage::game_skeleton_update_base* group_child_node = group->m_head; group_child_node; group_child_node =
			     group_child_node->m_next)
			{
				// TamperActions is a leftover from the old AC, but still useful to block anyway
				if (group_child_node->m_hash != 0xA0F39FB6 && group_child_node->m_hash != rage::constexprJoaat(
					"TamperActions"))
					continue;

				patched = true;
				//LOG(INFO) << "Patching problematic skeleton update";
				reinterpret_cast<rage::game_skeleton_update_element*>(group_child_node)->m_function =
					pointers::g_nullsub;
			}
			break;
		}
	}

	for (rage::skeleton_data& i : pointers::g_game_skeleton->m_sys_data)
	{
		if (i.m_hash != 0xA0F39FB6 && i.m_hash != rage::constexprJoaat("TamperActions"))
			continue;

		i.m_init_func = reinterpret_cast<uint64_t>(pointers::g_nullsub);
		i.m_shutdown_func = reinterpret_cast<uint64_t>(pointers::g_nullsub);
	}
	return patched;
}


void init()
{
	exceptions::init_exception_handler();
	thread_pool_instance = std::make_unique<thread_pool>();
	g_logger = std::make_unique<logger>(BRAND);
	pointers::scan_all();
	//Wait until game is loaded
	if (*pointers::g_loadingScreenState != eLoadingScreenState::Finished)
	{
		LOG(Info, "Waiting for game to load");
		while (*pointers::g_loadingScreenState != eLoadingScreenState::Finished)
		{
			std::this_thread::sleep_for(1000ms);
		}
	}
	// Disable anti-cheat skeleton
	while (!disable_anti_cheat_skeleton())
	{
		LOG(Warn, "Failed to patch anticheat gameskeleton. Trying again...");
		std::this_thread::sleep_for(500ms);
	}
	LOG(Info, "Disabled anticheat gameskeleton!");
	//Create rendering pointer
	renderer_instance = std::make_unique<renderer>();
	//Create and enable hooks. (Enable is handled in hooking::hooking)
	hooking_instance = std::make_unique<hooking>();
	hooking_instance->enable();
	//Create fibers
	g_pool.create();
	g_manager.add("commands", &commands::on_tick);
	g_manager.add("playerManager", &util::network::manager::onTick);
	g_manager.add("commandStream", &commands::engine::commandStreamTick);
	g_manager.add("script", &script::on_tick);
	//Create our own GTA thread, meant to replace a main hook
	create_thread(&g_manager);
	// DX Thread for drawing Menu
	g_dx_fiber_mgr.add(std::make_unique<fiber>([]
	{
		while (true)
		{
			ui::drawing::tick();
			image_loader::header_handler();
			ui::draw();
			fiber::current()->sleep();
		}
	}), "dS");
}

void loop()
{
	while (g_running)
	{
		if (GetAsyncKeyState(VK_END))
			g_running = false;
		std::this_thread::sleep_for(1000ms);
	}
}

void uninit()
{
	//Restore the GTA threads we allocated at init
	engine::cleanup_threads();
	//Cleanup fibers
	g_manager.destroy();
	g_dx_fiber_mgr.remove_all();
	//Disable hooks
	hooking_instance->disable();
	//Cleanup renderer
	renderer_instance.reset();
	//We wait a bit before we destory our hooking pointer because MinHook can be a bit slow.
	std::this_thread::sleep_for(350ms);
	hooking_instance.reset();
	thread_pool_instance->destroy();
	thread_pool_instance.reset();
	g_logger.reset();
	exceptions::uninit_exception_handler();
}

DWORD entry_point(LPVOID)
{
	init();
	loop();
	uninit();
	CloseHandle(g_thread);
	if (commands::g_exit_with_clean_up)
	{
		std::thread exit_thread_function(thread_function);
		exit_thread_function.join();
	}
	FreeLibraryAndExitThread(g_module, 0);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason_for_call, LPVOID)
{
	g_module = module;
	DisableThreadLibraryCalls(g_module);
	if (reason_for_call == DLL_PROCESS_ATTACH)
	{
		g_thread = CreateThread(nullptr, NULL, &entry_point, nullptr, NULL, g_main_thread);
	}
	return TRUE;
}
