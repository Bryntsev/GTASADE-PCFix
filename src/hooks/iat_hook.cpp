#include "hooks/iat_hook.h"

#include <TlHelp32.h>

#include <cstddef>
#include <cstring>

namespace sade {
namespace {

void* rva_to_ptr(HMODULE module, DWORD rva) {
  return reinterpret_cast<std::byte*>(module) + rva;
}

}  // namespace

std::size_t IatHookSet::install_all_loaded_modules(const char* imported_module,
                                                   const char* function_name,
                                                   void* replacement,
                                                   HMODULE skip_module) {
  std::lock_guard lock(mutex_);
  std::size_t installed = 0;
  const DWORD process_id = GetCurrentProcessId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return 0;
  }

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) {
    do {
      if (entry.hModule == skip_module) {
        continue;
      }
      installed += install_module(entry.hModule, imported_module, function_name, replacement) ? 1 : 0;
    } while (Module32NextW(snapshot, &entry));
  }

  CloseHandle(snapshot);
  return installed;
}

std::size_t IatHookSet::install_main_module(const char* imported_module,
                                             const char* function_name,
                                             void* replacement,
                                             HMODULE skip_module) {
  std::lock_guard lock(mutex_);
  HMODULE main_module = GetModuleHandleW(nullptr);
  if (main_module == skip_module) {
    return 0;
  }
  return install_module(main_module, imported_module, function_name, replacement) ? 1 : 0;
}

bool IatHookSet::install_module(HMODULE module, const char* imported_module, const char* function_name, void* replacement) {
  if (module == nullptr || imported_module == nullptr || function_name == nullptr || replacement == nullptr) {
    return false;
  }

  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return false;
  }

  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<std::byte*>(module) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return false;
  }

  const auto& import_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (import_dir.VirtualAddress == 0) {
    return false;
  }

  auto* descriptor = static_cast<IMAGE_IMPORT_DESCRIPTOR*>(rva_to_ptr(module, import_dir.VirtualAddress));
  for (; descriptor->Name != 0; ++descriptor) {
    const auto* dll_name = static_cast<const char*>(rva_to_ptr(module, descriptor->Name));
    if (_stricmp(dll_name, imported_module) != 0) {
      continue;
    }

    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(rva_to_ptr(module, descriptor->FirstThunk));
    auto* original_thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
        rva_to_ptr(module, descriptor->OriginalFirstThunk != 0 ? descriptor->OriginalFirstThunk : descriptor->FirstThunk));

    for (; original_thunk->u1.AddressOfData != 0; ++original_thunk, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL64(original_thunk->u1.Ordinal)) {
        continue;
      }
      auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(rva_to_ptr(module, static_cast<DWORD>(original_thunk->u1.AddressOfData)));
      if (std::strcmp(reinterpret_cast<const char*>(import->Name), function_name) != 0) {
        continue;
      }

      DWORD old_protect = 0;
      auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
      if (*slot == replacement || has_slot(slot)) {
        return false;
      }
      void* original = *slot;
      if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
        return false;
      }
      *slot = replacement;
      DWORD ignored = 0;
      VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
      FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
      slots_.push_back(Slot{slot, original});
      return true;
    }
  }

  return false;
}

bool IatHookSet::uninstall() {
  std::lock_guard lock(mutex_);
  bool ok = true;
  for (auto& slot : slots_) {
    if (slot.address == nullptr || slot.original == nullptr) {
      continue;
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(slot.address, sizeof(void*), PAGE_READWRITE, &old_protect)) {
      ok = false;
      continue;
    }
    *slot.address = slot.original;
    DWORD ignored = 0;
    VirtualProtect(slot.address, sizeof(void*), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot.address, sizeof(void*));
  }
  slots_.clear();
  return ok;
}

std::size_t IatHookSet::count() const {
  std::lock_guard lock(mutex_);
  return slots_.size();
}

bool IatHookSet::has_slot(void** slot) const {
  for (const auto& existing : slots_) {
    if (existing.address == slot) {
      return true;
    }
  }
  return false;
}

}  // namespace sade
