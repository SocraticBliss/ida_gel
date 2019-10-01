#include "../elf_common/elf_reader.h"
#include "cell_loader.h"
#include "sce.h"

#include <idaldr.h>

#include <memory>

#define DATABASE_FILE "ps3.xml"

static int idaapi 
accept_file(qstring *fileformatname, 
			qstring *processor, 
			linput_t *li, 
			const char *filename)
{
  elf_reader<elf64> elf(li);
  
  if (elf.verifyHeader() && 
	  elf.machine() == EM_PPC64 && 
	  elf.osabi() == ELFOSABI_CELLOSLV2) {
	const char *type;
	switch (elf.type())
	{
	case ET_EXEC:
	  type = "Executable";
	  break;
	case ET_SCE_PPURELEXEC:
	  type = "Relocatable Executable";
	  break;
	default:
	  return 0;
	}

	*processor = "ppc";
    fileformatname->sprnt("PlayStation 3 PPU (%s)", type);
    
    return 1;
  }
  
  return 0;
}

static void idaapi 
load_file(linput_t *li, 
		  ushort neflags, 
	      const char *fileformatname)
{
  set_processor_type("ppc", SETPROC_LOADER);
  elf_reader<elf64> elf(li);
  elf.read();
  
  ea_t relocAddr = 0;
  if (elf.type() == ET_SCE_PPURELEXEC) {
    if (neflags & NEF_MAN) {
      ask_addr(&relocAddr, "Please specify a relocation address base.");
    }
  }
  
  cell_loader ldr(&elf, relocAddr, DATABASE_FILE);
  ldr.apply();
}

extern "C"
__declspec(dllexport)						  
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