#include "../elf_common/elf_reader.h"
#include "cell_loader.h"
#include "sce.h"

#include <idaldr.h>

#include <memory>

#define DATABASE_FILE "ps3.xml"

static int idaapi 
 accept_file(qstring *fileformatname, qstring *processor, linput_t *li, const char *filename)
{
  elf_reader<elf64> elf(li);
   
  if (elf.verifyHeader() &&
      elf.machine() == EM_PPC64 &&
      elf.osabi() == ELFOSABI_CELLOSLV2) {
    const char *type;
  
    if (elf.type() == ET_EXEC)
      type = "Executable";
    else if (elf.type() == ET_SCE_PPURELEXEC)
      type = "Relocatable Executable";
    else
      return 0;

    set_processor_type("ppc", SETPROC_LOADER);
        
	fileformatname->sprnt("Playstation 3 PPU %s", type);
    
    return 1 | ACCEPT_FIRST;
  }
  
  return 0;
}

static void idaapi 
 load_file(linput_t *li, ushort neflags, const char *fileformatname)
{
  elf_reader<elf64> elf(li); elf.read();
    
  ea_t relocAddr = 0;
  if (elf.type() == ET_SCE_PPURELEXEC) {
    if (neflags & NEF_MAN) {
      ask_addr(&relocAddr, "Please specify a relocation address base.");
    }
  }

  cell_loader ldr(&elf, relocAddr, DATABASE_FILE); ldr.apply();
}

#ifdef _WIN32
__declspec(dllexport)
#endif
loader_t LDSC = 
{
  IDP_INTERFACE_VERSION,
  0,
  accept_file,
  load_file,
  NULL,
  NULL,
  NULL
};