// Loads game.bin, the game module the player built from their own ROM (an ARM ELF shared object): maps its segments,
// applies its relocations, binds its imports to the port's functions (the table host_exports.S, generated from
// port3ds/module/imports.txt), makes its code executable and runs its constructors.

#include <3ds.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <malloc.h>
#include <string>
#include <vector>

#include "module/rs3ds_module.h"

extern "C" const struct { const char* name; void* addr; } rs3ds_host_exports[];

namespace {

void* host_symbol(const char* name) {
    for (size_t i = 0; rs3ds_host_exports[i].name; i++) {
        if (std::strcmp(rs3ds_host_exports[i].name, name) == 0) return rs3ds_host_exports[i].addr;
    }
    return nullptr;
}

bool fail(char* err, size_t n, const std::string& msg) {
    std::snprintf(err, n, "%s", msg.c_str());
    return false;
}

} // namespace

const RS3DSModule* rs3ds_load_module(const char* path, char* err, size_t err_size) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        fail(err, err_size, "missing");
        return nullptr;
    }
    std::fseek(f, 0, SEEK_END);
    const long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file((size_t)fsize);
    const bool read_ok = fsize > (long)sizeof(Elf32_Ehdr) && std::fread(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    const auto* eh = (const Elf32_Ehdr*)file.data();
    if (!read_ok || std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_machine != EM_ARM || eh->e_type != ET_DYN) {
        fail(err, err_size, "not a game module");
        return nullptr;
    }
    const auto* ph = (const Elf32_Phdr*)(file.data() + eh->e_phoff);

    // one block for all loadable segments, page aligned
    uint32_t top = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_TLS) {
            fail(err, err_size, "module uses thread-local storage");
            return nullptr;
        }
        if (ph[i].p_type == PT_LOAD) top = std::max<uint32_t>(top, ph[i].p_vaddr + ph[i].p_memsz);
    }
    const uint32_t size = (top + 0xFFF) & ~0xFFFu;
    uint8_t* base = (uint8_t*)memalign(0x1000, size);
    if (!base) {
        fail(err, err_size, "not enough memory for the game module");
        return nullptr;
    }
    std::memset(base, 0, size);
    const Elf32_Dyn* dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) std::memcpy(base + ph[i].p_vaddr, file.data() + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_type == PT_DYNAMIC) dyn = (const Elf32_Dyn*)(base + ph[i].p_vaddr);
    }
    if (!dyn) {
        fail(err, err_size, "module has no dynamic section");
        return nullptr;
    }

    const Elf32_Sym* symtab = nullptr;
    const char* strtab = nullptr;
    const Elf32_Rel* rel = nullptr;
    const Elf32_Rel* jmprel = nullptr;
    uint32_t relsz = 0, pltrelsz = 0, init_array = 0, init_arraysz = 0, init = 0, nsyms = 0;
    for (const Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = (const Elf32_Sym*)(base + d->d_un.d_ptr); break;
        case DT_STRTAB: strtab = (const char*)(base + d->d_un.d_ptr); break;
        case DT_REL: rel = (const Elf32_Rel*)(base + d->d_un.d_ptr); break;
        case DT_RELSZ: relsz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = (const Elf32_Rel*)(base + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_INIT_ARRAY: init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: init_arraysz = d->d_un.d_val; break;
        case DT_INIT: init = d->d_un.d_ptr; break;
        case DT_HASH: nsyms = ((const uint32_t*)(base + d->d_un.d_ptr))[1]; break;
        case DT_RELA:
            fail(err, err_size, "module uses RELA relocations");
            return nullptr;
        default: break;
        }
    }
    if (!symtab || !strtab) {
        fail(err, err_size, "module has no symbol table");
        return nullptr;
    }

    std::string missing;
    auto apply = [&](const Elf32_Rel* r, uint32_t bytes) {
        for (uint32_t i = 0; i < bytes / sizeof(Elf32_Rel); i++) {
            uint32_t* where = (uint32_t*)(base + r[i].r_offset);
            const uint32_t type = ELF32_R_TYPE(r[i].r_info), si = ELF32_R_SYM(r[i].r_info);
            uint32_t s = 0;
            if (si) {
                const Elf32_Sym& sym = symtab[si];
                if (sym.st_shndx != SHN_UNDEF) {
                    s = (uint32_t)(uintptr_t)base + sym.st_value;
                } else {
                    void* h = host_symbol(strtab + sym.st_name);
                    if (!h && ELF32_ST_BIND(sym.st_info) != STB_WEAK) {
                        if (missing.size() < 200) missing += std::string(missing.empty() ? "" : ", ") + (strtab + sym.st_name);
                        continue;
                    }
                    s = (uint32_t)(uintptr_t)h;
                }
            }
            switch (type) {
            case R_ARM_NONE: break;
            case R_ARM_RELATIVE: *where += (uint32_t)(uintptr_t)base; break;
            case R_ARM_ABS32: *where += s; break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: *where = s; break;
            default:
                if (missing.size() < 200) missing += std::string(missing.empty() ? "" : ", ") + "relocation type " + std::to_string(type);
                break;
            }
        }
    };
    if (rel) apply(rel, relsz);
    if (jmprel) apply(jmprel, pltrelsz);
    if (!missing.empty()) {
        fail(err, err_size, "this rs64.3dsx is too old for the game module (missing: " + missing + ")");
        return nullptr;
    }

    // code pages read + execute, the rest stays read/write
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)base, size);
    Handle proc = 0;
    if (R_FAILED(svcDuplicateHandle(&proc, CUR_PROCESS_HANDLE))) {
        fail(err, err_size, "no process handle");
        return nullptr;
    }
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        const uint32_t a = (uint32_t)(uintptr_t)base + (ph[i].p_vaddr & ~0xFFFu);
        const uint32_t e = ((uint32_t)(uintptr_t)base + ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~0xFFFu;
        const Result r = svcControlProcessMemory(proc, a, a, e - a, MEMOP_PROT, MEMPERM_READ | MEMPERM_EXECUTE);
        if (R_FAILED(r)) {
            svcCloseHandle(proc);
            char m[96];
            std::snprintf(m, sizeof m, "could not make the game code executable (0x%08lX): Luma3DS is needed", (unsigned long)r);
            fail(err, err_size, m);
            return nullptr;
        }
    }
    svcCloseHandle(proc);

    // constructors, then the entry point
    if (init) ((void (*)())(base + init))();
    for (uint32_t i = 0; i < init_arraysz / 4; i++) {
        const uint32_t fn = ((const uint32_t*)(base + init_array))[i];
        if (fn && fn != 0xFFFFFFFFu) ((void (*)())(uintptr_t)fn)();
    }
    const RS3DSModule* (*entry)() = nullptr;
    for (uint32_t i = 1; i < nsyms; i++) {
        const Elf32_Sym& sym = symtab[i];
        if (sym.st_shndx != SHN_UNDEF && std::strcmp(strtab + sym.st_name, "rs3ds_module_entry") == 0) {
            entry = (const RS3DSModule* (*)())(base + sym.st_value);
            break;
        }
    }
    if (!entry) {
        fail(err, err_size, "module has no entry point");
        return nullptr;
    }
    const RS3DSModule* m = entry();
    std::fprintf(stderr, "[module] %s loaded at %p (%lu KB), build %s, ABI %lu\n", path, base, (unsigned long)(size >> 10),
                 m ? m->build_id : "?", m ? (unsigned long)m->abi : 0ul);
    if (!m || m->abi != RS3DS_MODULE_ABI) {
        fail(err, err_size, "the game module was built for another version: run the setup on your computer again");
        return nullptr;
    }
    return m;
}
