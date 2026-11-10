#pragma once

namespace boot
{
	void on_dll_attach(HMODULE module);

	// called from the XInput exports and the watcher thread; cheap after initialization
	void try_start();

	[[nodiscard]] HMODULE get_module();
	[[nodiscard]] bool is_initialized();

	// true when the host client already contains its own demo system (e.g. the anomaly fork)
	[[nodiscard]] bool host_has_demo_system();
}
