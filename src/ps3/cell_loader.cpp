#include "cell_loader.hpp"

#include <idaldr.h>
#include <struct.hpp>

#include <memory>
#include <vector>

cell_loader::cell_loader(elf_reader<elf64> *elf, 
                         uint64 relocAddr, 
                         std::string databaseFile)
  : m_elf(elf)
{
  m_hasSegSym = false;
  m_relocAddr = 0;
  
  // only PRX's contain relocations
  if ( isLoadingPrx() )
    m_relocAddr = relocAddr;
  
  inf.demnames |= DEMNAM_GCC3;  // assume gcc3 names
  inf.af       |= AF_PROCPTR;   // Create function if data xref data->code32 exists
  inf.filetype = f_ELF;
  
  char databasePath[QMAXPATH];
    
  if ( getsysfile(databasePath, QMAXFILE, databaseFile.c_str(), LDR_SUBDIR) == NULL )
    loader_failure("Could not locate database file (%s).\n", databaseFile.c_str());
  
  if ( m_database.LoadFile(databasePath) == false )
    loader_failure("Failed to load database file (%s).\n", databaseFile.c_str());
}

void cell_loader::apply() {
  msg("Declaring Structures...\n");
  declareStructures();
  
  msg("Applying Segments...\n");
  applySegments();
  
  msg("Swapping Symbols...\n");
  swapSymbols();
  
  if ( isLoadingPrx() ) {
    // the only way I know to check if its a 0.85 PRX
    for ( auto &segment : m_elf->getSegments() ) {
      if ( segment.p_type == PT_SCE_SEGSYM ) {
        m_hasSegSym = true;
        break;
      }
    }
    
    // we need gpValue for relocations on 0.85
    // otherwise, I don't think newer PRX's have
    // TOC based relocations. TOC is not set in 
    // moduleInfo. It seems to always be zero.
    if ( m_hasSegSym ) {
      //msg("Looking for .toc section\n");
      auto tocSection = m_elf->getSectionByName(".toc");
      if ( tocSection ) {
        //msg("Found toc section!\n");
        m_gpValue = tocSection->sh_addr + m_relocAddr;
      }
    }
    
    // gpValue can be found at sceModuleInfo->gp_value
    // 0.85 gpValue is base address of .toc
    msg("Applying Relocations...\n");
    applyRelocations();
    
    // if not a 0.85 PRX
    if ( !m_hasSegSym ) {
      // p_paddr is an offset into the file.
      auto firstSegment = m_elf->getSegments()[0];
      m_gpValue = get_dword( (firstSegment.p_vaddr + m_relocAddr) + // file offset to address
                            (firstSegment.p_paddr - firstSegment.p_offset) +
                             offsetof(_scemoduleinfo_ppu32, gp_value) );
    }
    
    msg("Applying Module Info...\n");
    applyModuleInfo();
  } else if ( isLoadingExec() ) {
    // gpValue can be found at m_elf->entry() + 4
    // _start is actually what loads TOC which is hardcoded to lwz(entry + 4)
    // there are also function stubs which set TOC to a different value
    m_gpValue = get_dword(m_elf->entry() + 4);
    
    applyProcessInfo();
    
    add_entry(0, m_elf->entry(), "_start", true);
  }
  
  msg("gpValue = %08x\n", m_gpValue);
  
  // set TOC in IDA
  ph.notify(processor_t::event_t(ph.ev_loader+1), m_gpValue);
  
  // we want to apply the symbols last so that symbols
  // always override our own custom symbols.
  msg("Applying Symbols...\n");
  applySymbols();
}

void cell_loader::applySegments() {
  // we prefer section headers
  if ( m_elf->getNumSections() > 0 )
    applySectionHeaders();
  // otherwise load program headers
  else if ( m_elf->getNumSegments() > 0 )
    applyProgramHeaders();
  else
    loader_failure("No segments available!");
}

void cell_loader::applySectionHeaders() {
  msg("Applying section headers...\n");
  auto &sections = m_elf->getSections();
  const char *strTab = m_elf->getSectionStringTable()->data();
  
  size_t index = 0;
  for ( const auto &section : sections ) {
    // only load allocatable sections
    if ( section.sh_flags & SHF_ALLOC &&
        section.sh_size > 0 ) {
      if ( section.sh_type == SHT_NULL )
        continue;
      
      uchar perm = 0;
      char *sclass = NULL;
      
      if ( section.sh_flags & SHF_WRITE )
        perm |= SEGPERM_WRITE;
      if ( section.sh_flags & SHF_EXECINSTR )
        perm |= SEGPERM_EXEC;
      
      if ( section.sh_flags & SHF_EXECINSTR )
        sclass = CLASS_CODE;
      else if ( section.sh_type == SHT_NOBITS )
        sclass = CLASS_BSS;
      else
        sclass = CLASS_DATA;
      
      const char *name = NULL;
      if ( section.sh_name != NULL )
        name = &strTab[section.sh_name];
      
      applySegment( index, 
                    section.sh_offset, 
                    section.sh_addr, 
                    section.sh_size, 
                    name, 
                    sclass, 
                    perm, 
                    m_elf->getAlignment(section.sh_addralign), 
                    (section.sh_type == SHT_NOBITS) ? false : true );
      
      ++index;
    }
  }
}

void cell_loader::applyProgramHeaders() {
  msg("Applying program headers...\n");
  auto &segments = m_elf->getSegments();
  
  size_t index = 0;
  for ( const auto &segment : segments ) {
    if ( segment.p_memsz > 0 ) {
      uchar perm;
      char *sclass;
      
      if ( segment.p_flags & PF_W )    // if its writable
        sclass = CLASS_DATA;
      if ( (segment.p_flags & PF_R) && // if its only readable
          !(segment.p_flags & PF_W) &&
          !(segment.p_flags & PF_X) )
        sclass = CLASS_CONST;
      if ( segment.p_flags & PF_X )    // if its executable
        sclass = CLASS_CODE;
      
      if ( segment.p_filesz == 0 &&
           segment.p_memsz > 0 )
        sclass = CLASS_BSS;
      
      if ( segment.p_flags & PF_X )
        perm |= SEGPERM_EXEC;
      if ( segment.p_flags & PF_W )
        perm |= SEGPERM_WRITE;
      if ( segment.p_flags & PF_R )
        perm |= SEGPERM_READ;
      
      applySegment( index, 
                    segment.p_offset, 
                    segment.p_vaddr, 
                    segment.p_memsz, 
                    NULL, 
                    sclass, 
                    perm, 
                    m_elf->getAlignment(segment.p_align) );
      
      ++index;
    }
  }
}

void cell_loader::applySegment(uint32 sel, 
                               uint64 offset, 
                               uint64 addr, 
                               uint64 size, 
                               const char *name, 
                               const char *sclass, 
                               uchar perm, 
                               uchar align, 
                               bool load) {
  addr += m_relocAddr;
  
  segment_t seg;
  seg.start_ea = addr;
  seg.end_ea = addr + size;
  seg.color = DEFCOLOR;
  seg.sel = sel;
  seg.bitness = 1;
  seg.orgbase = sel;
  seg.comb = scPub;
  seg.perm = perm;
  seg.flags = SFL_LOADER;
  seg.align = align;
  
  set_selector(sel, 0);
  
  if ( name == NULL )
    name = "";
  
  add_segm_ex(&seg, name, sclass, NULL);
  
  if ( load == true )
    file2base(m_elf->getReader(), offset, addr, addr + size, true);
}

void cell_loader::applyRelocations() {
  if ( m_hasSegSym )
    applySectionRelocations();  // pretty much only for 0.85
  else
    applySegmentRelocations();
}

void cell_loader::applySectionRelocations() {
  msg("Applying section based relocations..\n");
  
  auto &sections = m_elf->getSections();
  auto symbols = m_elf->getSymbols();
  
  for ( auto &section : sections ) {
    // NOTE: the only SHT_RELA sections I see after 0.85 
    //       are non-allocatable so no reason to consider those
    if ( section.sh_type == SHT_RELA ) {
      if ( !(sections[ section.sh_info ].sh_flags & SHF_ALLOC) )
        continue;
      
      auto nrela = section.sh_size / sizeof(Elf64_Rela);
      auto relocations = reinterpret_cast<Elf64_Rela *>(section.data());
      
      for ( size_t i = 0; i < nrela; ++i ) {
        auto &rela = relocations[i];
        
        swap(rela.r_offset);
        swap(rela.r_info);
        swap(rela.r_addend);
        
        uint32 type = ELF64_R_TYPE(rela.r_info);
        uint32 sym  = ELF64_R_SYM (rela.r_info);
        
        //msg("r_type: %08x\n", type);
        //msg("r_sym: %08x\n", sym);
        
        if ( type == R_PPC64_NONE ) {
          msg("Skipping relocation..\n");
          continue;
        }
        
        if ( type > R_PPC64_TLSGD ) {
          msg("Invalid relocation type (%i)!\n", type);
          continue;
        }
        
        //msg("nsyms = %08x\n", m_elf->getNumSymbols());
        //msg("symsec = %04x\n", symbols[ sym ].st_shndx);
        
        if ( sym > m_elf->getNumSymbols() ) {
          msg("Invalid symbol index!\n");
          continue;
        }
        
        if ( symbols[ sym ].st_shndx > m_elf->getNumSections() ) {
          if ( symbols[ sym ].st_shndx != SHN_ABS ) {
            msg("Invalid symbol section index!\n");
            continue;
          }
        }
        
        uint32 symaddr;
        if ( symbols[ sym ].st_shndx == SHN_ABS )
          symaddr = symbols[ sym ].st_value;
        else
          symaddr = sections[ symbols[ sym ].st_shndx ].sh_addr;
        
        uint32 addr  = sections[ section.sh_info ].sh_addr + 
                       rela.r_offset;
        uint32 saddr = symaddr + symbols[ sym ].st_value + 
                       rela.r_addend;
        
        applyRelocation(type, addr, saddr);
      }
    }
  }
}

void cell_loader::applySegmentRelocations() {
  msg("Applying segment based relocations..\n");
  
  auto &segments = m_elf->getSegments();
  
  for ( auto &segment : segments ) {
    if ( segment.p_type == PT_SCE_PPURELA ) {
      auto nrela = segment.p_filesz / sizeof(Elf64_Rela);
      auto relocations = reinterpret_cast<Elf64_Rela *>(segment.data());
      
      for ( size_t i = 0; i < nrela; ++i ) {
        auto &rela = relocations[i];
        
        swap(rela.r_offset);
        swap(rela.r_info);
        swap(rela.r_addend);
        
        auto type     = ELF64_R_TYPE(rela.r_info);
        
        if ( type == R_PPC64_NONE )
          continue;
        
        auto sym    = ELF64_R_SYM(rela.r_info);
        auto patchseg = (sym & 0x000000ff);
        auto symseg   = (sym & 0x7fffff00) >> 8;
        
        uint32 addr, saddr;
        if ( patchseg == 0xFF )
          addr = 0;
        else
          addr = segments[patchseg].p_vaddr + rela.r_offset;
        
        if ( symseg == 0xFF )
          saddr = 0;
        else
          saddr = segments[symseg].p_vaddr + rela.r_addend;
        
        applyRelocation(type, addr, saddr);
      }
      break;  // TODO: should only be one segment right?
    }
  }
}

void cell_loader::applyRelocation(uint32 type, uint32 addr, uint32 saddr) {
  uint32 value;
  
  addr += m_relocAddr;
  saddr += m_relocAddr;
  
  //msg("Applying relocation %i (%08x -> %08x)\n", type, addr, saddr);
  
  switch ( type ) {
    case R_PPC64_ADDR32:
      value = saddr;
      patch_dword(addr, value);
      break;
    case R_PPC64_ADDR16_LO:
      value = saddr & 0xFFFF;
      patch_word(addr, value);
      break;
    case R_PPC64_ADDR16_HA:
      value = (((saddr + 0x8000) >> 16) & 0xFFFF);
      patch_word(addr, value);
      break;
    case R_PPC64_REL24:
      value = get_original_dword(addr);
      value = (value & ~0x03fffffc) | ((saddr - addr) & 0x3fffffc);
      patch_dword(addr, value);
      break;
    case R_PPC64_TOC16:
      value = saddr - m_gpValue;
      patch_word(addr, value);
      break;
    case R_PPC64_TOC16_DS:
      value = get_word(addr);
      value = (value & ~0xFFFC) | ((saddr - m_gpValue) & 0xFFFC);
      patch_word(addr, value);
      break;
    case R_PPC64_TLSGD:
      value = m_gpValue;
      patch_dword(addr, value);
      break;
    default:
      msg("Unsupported relocation (%i).\n", type);
      break;
  }
}

void cell_loader::loadExports(uint32 entTop, uint32 entEnd) {
  msg("Loading exports...\n");
  
  tid_t tid = get_struc_id("_scelibent_ppu32");
  force_name(entTop - 4, "__begin_of_section_lib_ent");
  force_name(entEnd, "__end_of_section_lib_ent");
  
  uchar structsize;
  
  for ( ea_t ea = entTop; ea < entEnd; ea += structsize ) {
    structsize = get_byte(ea);
    
    auto nfunc   = get_word(ea + offsetof(_scelibent_common, nfunc));
    auto nvar    = get_word(ea + offsetof(_scelibent_common, nvar));
    auto ntlsvar = get_word(ea + offsetof(_scelibent_common, ntlsvar));
    auto count = nfunc + nvar + ntlsvar;
    
    //msg("Num Functions: %i\n", nfunc);
    //msg("Num Variables: %i\n", nvar);
    //msg("Num TLS Variables: %i\n", ntlsvar);
    
    if ( structsize == sizeof(_scelibent_ppu32) ) {
      create_struct(ea, sizeof(_scelibent_ppu32), tid);
      
      auto libNamePtr = get_dword(ea + offsetof(_scelibent_ppu32, libname));
      auto nidTable   = get_dword(ea + offsetof(_scelibent_ppu32, nidtable));
      auto addTable   = get_dword(ea + offsetof(_scelibent_ppu32, addtable));
      
      qstring libName;
      char symName[MAXNAMELEN];
      if ( libNamePtr == NULL ) {
        force_name(nidTable, "_NONAMEnid_table");
        force_name(addTable, "_NONAMEentry_table");
      } else {
        get_strlit_contents(&libName, libNamePtr, get_max_strlit_length(libNamePtr, STRTYPE_C), STRTYPE_C);
        
        qsnprintf(symName, MAXNAMELEN, "_%s_str", libName.c_str());
        force_name(libNamePtr, symName);
        
        qsnprintf(symName, MAXNAMELEN, "__%s_Functions_NID_table", libName.c_str());
        force_name(nidTable, symName);
        
        qsnprintf(symName, MAXNAMELEN, "__%s_Functions_table", libName.c_str());
        force_name(addTable, symName);
      }
      
      //msg("Processing entries..\n");
      if ( nidTable != NULL && addTable != NULL ) {
        for ( int i = 0; i < count; ++i ) {
          const char *resolvedNid;
          ea_t nidOffset = nidTable + (i * 4);
          ea_t addOffset = addTable + (i * 4);
          
          uint32 nid = get_dword(nidOffset);
          uint32 add = get_dword(addOffset);
          
          if ( libNamePtr ) {
            uint32 addToc = get_dword(add);
            resolvedNid = getNameFromDatabase(libName.c_str(), nid);
            if ( resolvedNid ) {
              set_cmt(nidOffset, resolvedNid, false);
              force_name(add, resolvedNid);
              
              // only label functions this way
              if ( i < nfunc ) {
                qsnprintf(symName, MAXNAMELEN, ".%s", resolvedNid);
                force_name(addToc, symName);
              }
            }
            
            if ( i < nfunc )
              auto_make_proc(addToc);
          }
          
          //msg("create_dword: %08x\n", nidOffset);
          //msg("create_dword: %08x\n", addOffset);
          create_dword(nidOffset, 4);
          create_dword(addOffset, 4);
        }
      }
    } else {
      msg("Unknown export structure at %08x.\n", ea);
      continue;
    }
  }
}

void cell_loader::loadImports(uint32 stubTop, uint32 stubEnd) {
  msg("Loading imports...\n");
  
  tid_t tid = get_struc_id("_scelibstub_ppu32");
  
  force_name(stubTop - 4, "__begin_of_section_lib_stub");
  force_name(stubEnd, "__end_of_section_lib_stub");
  
  uchar structsize;
  
  // define data for lib stub
  for ( ea_t ea = stubTop; ea < stubEnd; ea += structsize ) {
    structsize = get_byte(ea);
    
    auto nFunc   = get_word(ea + offsetof(_scelibstub_common, nfunc));
    auto nVar    = get_word(ea + offsetof(_scelibstub_common, nvar));
    auto nTlsVar = get_word(ea + offsetof(_scelibstub_common, ntlsvar));
    
    //msg("Num Functions: %i\n", nFunc);
    //msg("Num Variables: %i\n", nVar);
    //msg("Num TLS Variables: %i\n", nTlsVar);
    
    if (structsize == sizeof(_scelibstub_ppu32)) {
      create_struct(ea, sizeof(_scelibstub_ppu32), tid);
      
      ea_t libNamePtr   = get_dword(ea + offsetof(_scelibstub_ppu32, libname));
      ea_t funcNidTable = get_dword(ea + offsetof(_scelibstub_ppu32, func_nidtable));
      ea_t funcTable    = get_dword(ea + offsetof(_scelibstub_ppu32, func_table));
      ea_t varNidTable  = get_dword(ea + offsetof(_scelibstub_ppu32, var_nidtable));
      ea_t varTable     = get_dword(ea + offsetof(_scelibstub_ppu32, var_table));
      ea_t tlsNidTable  = get_dword(ea + offsetof(_scelibstub_ppu32, tls_nidtable));
      ea_t tlsTable     = get_dword(ea + offsetof(_scelibstub_ppu32, tls_table));
      
      qstring libName;
      char symName[MAXNAMELEN];
      get_strlit_contents(&libName, libNamePtr, get_max_strlit_length(libNamePtr, STRTYPE_C), STRTYPE_C);
      
      qsnprintf(symName, MAXNAMELEN, "_%s_0001_stub_head", libName.c_str());
      force_name(ea, symName);
      
      qsnprintf(symName, MAXNAMELEN, "_%s_stub_str", libName.c_str());
      force_name(libNamePtr, symName);
      
      qsnprintf(symName, MAXNAMELEN, "_sce_package_version_%s", libName.c_str());
      force_name(libNamePtr - 4, symName);
      
      //msg("Processing %i exported functions...\n", nFunc);
      if ( funcNidTable != NULL && funcTable != NULL ) {
        for ( int i = 0; i < nFunc; ++i ) {
          const char *resolvedNid;
          
          ea_t nidOffset = funcNidTable + (i * 4);
          ea_t funcOffset = funcTable + (i * 4);
          
          uint32 nid = get_dword(nidOffset);
          uint32 func = get_dword(funcOffset);
          
          resolvedNid = getNameFromDatabase(libName.c_str(), nid);
          if ( resolvedNid ) {
            set_cmt(nidOffset, resolvedNid, false);
            qsnprintf(symName, MAXNAMELEN, "%s.stub_entry", resolvedNid);
            force_name(funcOffset, symName);
            qsnprintf(symName, MAXNAMELEN, ".%s", resolvedNid);
            force_name(func, symName);
            
            netnode import_node;
            netnode_check(&import_node, libName.c_str(), 0, true); //"$ IDALDR node for ids loading $"
            netnode_supset(import_node, func, symName, 0, 339);
            import_module(libName.c_str(), 0, import_node, 0, "linux");
          }
          
          //msg("create_dword: %08x\n", nidOffset);
          //msg("create_dword: %08x\n", funcOffset);
          create_dword(nidOffset, 4);   // nid
          create_dword(funcOffset, 4);  // func
          /*if ( add_func(func, BADADDR) ) {
            get_func(func)->flags |= FUNC_LIB;
            //add_entry(func, func, ...)
          }*/
        }
      }
      
      //msg("Processing exported variables...\n");
      if ( varNidTable != NULL && varTable ) {
        for ( int i = 0; i < nVar; ++i ) {
          const char *resolvedNid;
          
          ea_t nidOffset = varNidTable + (i * 4);
          ea_t varOffset = varTable + (i * 4);
          
          uint32 nid = get_dword(nidOffset);
          uint32 func = get_dword(varOffset);
          
          resolvedNid = getNameFromDatabase(libName.c_str(), nid);
          if ( resolvedNid ) {
            set_cmt(nidOffset, resolvedNid, false);
            force_name(varOffset, resolvedNid);
          }
          
          //msg("create_dword: %08x\n", nidOffset);
          //msg("create_dword: %08x\n", varOffset);
          create_dword(nidOffset, 4);
          create_dword(varOffset, 4);
        }
      }
      
      //msg("Processing exported TLS variables...\n");
      if ( tlsNidTable != NULL && tlsTable != NULL ) {
        for ( int i = 0; i < nVar; ++i ) {
          const char *resolvedNid;
          
          ea_t nidOffset = tlsNidTable + (i * 4);
          ea_t tlsOffset = tlsTable + (i * 4);
          
          uint32 nid = get_dword(nidOffset);
          uint32 func = get_dword(tlsOffset);
          
          resolvedNid = getNameFromDatabase(libName.c_str(), nid);
          if ( resolvedNid ) {
            set_cmt(nidOffset, resolvedNid, false);
            force_name(tlsOffset, resolvedNid);
          }
          
          //msg("create_dword: %08x\n", nidOffset);
          //msg("create_dword: %08x\n", tlsOffset);
          create_dword(nidOffset, 4);
          create_dword(tlsOffset, 4);
        }
      }
    } else {
      msg("Unknown import structure at %08x.\n", ea);
      continue;
    }
  }
}

const char *cell_loader::getNameFromDatabase(
    const char *library, unsigned int nid) {
  auto header = m_database.FirstChildElement();
  
  if ( header ) {
    auto group  = header->FirstChildElement(); 
    if ( group ) {
      // find library in xml
      do {
        if ( !strcmp(library, group->Attribute("name")) ) {
          auto entry = group->FirstChildElement();
          if ( entry ) {
            // find NID in library group
            do {
              if ( strtoul(entry->Attribute("id"),0,0) == nid )
                  return entry->Attribute("name");
            } while ( entry = entry->NextSiblingElement() );
          }
        }
      } while ( group = group->NextSiblingElement() );
    }
  }

  return nullptr;
}

void cell_loader::applyModuleInfo() {
  auto firstSegment = m_elf->getSegments()[0];
  
  ea_t modInfoEa = (firstSegment.p_vaddr + m_relocAddr) +
                   (firstSegment.p_paddr - firstSegment.p_offset);
                             
  tid_t tid = get_struc_id("_scemoduleinfo");
  create_struct(modInfoEa, sizeof(_scemoduleinfo_ppu32), tid);
  
  loadExports( get_dword(modInfoEa + offsetof(_scemoduleinfo_ppu32, ent_top)),
               get_dword(modInfoEa + offsetof(_scemoduleinfo_ppu32, ent_end)) );
               
  loadImports( get_dword(modInfoEa + offsetof(_scemoduleinfo_ppu32, stub_top)),
               get_dword(modInfoEa + offsetof(_scemoduleinfo_ppu32, stub_end)) );
  
  add_entry(0, modInfoEa, "module_info", false);
                             
}

void cell_loader::applyProcessInfo() {
  for ( auto segment : m_elf->getSegments() ) {
    if ( segment.p_type == PT_PROC_PARAM ) {
      tid_t tid = get_struc_id("sys_process_param_t");
      create_struct(segment.p_vaddr, sizeof(sys_process_param_t), tid);
    } else if ( segment.p_type == PT_PROC_PRX ) {
      tid_t tid = get_struc_id("sys_process_prx_info_t");
      create_struct(segment.p_vaddr, sizeof(sys_process_prx_info_t), tid);
      
      loadExports( get_dword(segment.p_vaddr + offsetof(sys_process_prx_info_t, libent_start)),
                   get_dword(segment.p_vaddr + offsetof(sys_process_prx_info_t, libent_end)) );
      
      loadImports( get_dword(segment.p_vaddr + offsetof(sys_process_prx_info_t, libstub_start)),
                   get_dword(segment.p_vaddr + offsetof(sys_process_prx_info_t, libstub_end)) );
    }
  }
}

void cell_loader::swapSymbols() {
  // Since section based relocations depend on symbols 
  // we need to swap symbols before we get to relocations.
  // Pretty much only for 0.85 PRX's but we still need to
  // swap them anyway.
  auto section = m_elf->getSymbolsSection();
  
  if (section == NULL)
    return;
  
  //msg("Swapping symbols...\n");
  
  auto symbols = m_elf->getSymbols();
  
  for ( size_t i = 0; i < m_elf->getNumSymbols(); ++i ) {
    auto symbol = &symbols[i];
    
    swap(symbol->st_name);
    swap(symbol->st_shndx);
    swap(symbol->st_size);
    swap(symbol->st_value);
  }
}

void cell_loader::applySymbols() {
  auto section = m_elf->getSymbolsSection();
  
  if (section == NULL)
    return;
  
  msg("Applying symbols...\n");
  
  auto nsym = m_elf->getNumSymbols();
  auto symbols = m_elf->getSymbols();
  
  const char *stringTable = m_elf->getSections().at(section->sh_link).data();
  
  for ( size_t i = 0; i < nsym; ++i ) {
    auto &symbol = symbols[i];
    
    auto type = ELF64_ST_TYPE(symbol.st_info),
         bind = ELF64_ST_BIND(symbol.st_info);
    auto value = symbol.st_value;
    
    //msg("st_name: %08x\n", symbol.st_name);
    //msg("st_type: %08x\n", type);
    //msg("st_bind: %08x\n", bind);
    
    if ( symbol.st_shndx > m_elf->getNumSections() ||
        !(m_elf->getSections()[ symbol.st_shndx ].sh_flags & SHF_ALLOC) )
      continue;
    
    if ( symbol.st_shndx == SHN_ABS )
      continue;
    
    if ( isLoadingPrx() )
      value += m_elf->getSections()[ symbol.st_shndx ].sh_addr + m_relocAddr;
    
    switch ( type ) {
    case STT_OBJECT:
      force_name(value, &stringTable[ symbol.st_name ]);
      break;
    case STT_FILE:
      add_extra_line(value, true, "Source File: %s", &stringTable[ symbol.st_name ]);
      break;
    case STT_FUNC:
      force_name(value, &stringTable[ symbol.st_name ]);
      auto_make_proc(value);
      break;
    default:
      break;
    }
  }
}

void cell_loader::declareStructures() {
  struc_t *sptr;
  
  // offset type
  opinfo_t ot;
  ot.ri.flags   = REF_OFF32;
  ot.ri.target  = BADADDR;
  ot.ri.base    = 0;
  ot.ri.tdelta  = 0;
  
  tid_t modInfoCommon = add_struc(BADADDR, "_scemoduleinfo_common");
  sptr = get_struc(modInfoCommon);
  if ( sptr != NULL ) {
    add_struc_member(sptr, "modattribute", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "modversion", BADADDR, byte_flag(), NULL, 2);
    add_struc_member(sptr, "modname", BADADDR, byte_flag(), NULL, SYS_MODULE_NAME_LEN);
    add_struc_member(sptr, "terminal", BADADDR, byte_flag(), NULL, 1);
    
    sptr = get_struc(add_struc(BADADDR, "_scemoduleinfo"));
    if ( sptr != NULL ) {
      opinfo_t mt;
      mt.tid = modInfoCommon;
      add_struc_member(sptr, "c", BADADDR, stru_flag(), &mt, get_struc_size(mt.tid));
      
      add_struc_member(sptr, "gp_value", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "ent_top", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "ent_end", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "stub_top", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "stub_end", BADADDR, off_flag() | dword_flag(), &ot, 4);
    }
  }
  
  tid_t libStubCommon = add_struc(BADADDR, "_scelibstub_ppu_common");
  sptr = get_struc(libStubCommon);
  if ( sptr != NULL ) {
    add_struc_member(sptr, "structsize", BADADDR, byte_flag(), NULL, 1);
    add_struc_member(sptr, "reserved1", BADADDR, byte_flag(), NULL, 1);
    add_struc_member(sptr, "version", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "attribute", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "nfunc", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "nvar", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "ntlsvar", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "reserved2", BADADDR, byte_flag(), NULL, 4);
    
    sptr = get_struc(add_struc(BADADDR, "_scelibstub_ppu32"));
    if ( sptr != NULL ) {
      opinfo_t mt;
      mt.tid = libStubCommon;
      add_struc_member(sptr, "c", BADADDR, stru_flag(), &mt, get_struc_size(mt.tid));
      
      add_struc_member(sptr, "libname", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "func_nidtable", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "func_table", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "var_nidtable", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "var_table", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "tls_nidtable", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "tls_table", BADADDR, off_flag() | dword_flag(), &ot, 4);
    }
  }
  
  tid_t libEntCommon = add_struc(BADADDR, "_scelibent_ppu_common");
  sptr = get_struc(libEntCommon);
  if ( sptr != NULL ) {
    add_struc_member(sptr, "structsize", BADADDR, byte_flag(), NULL, 1);
    add_struc_member(sptr, "reserved1", BADADDR, byte_flag(), NULL, 1);
    add_struc_member(sptr, "version", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "attribute", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "nfunc", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "nvar", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "ntlsvar", BADADDR, word_flag(), NULL, 2);
    add_struc_member(sptr, "hashinfo", BADADDR, byte_flag(), NULL, 1);
    add_struc_member(sptr, "hashinfotls", BADADDR, byte_flag(), NULL, 1);
    add_struc_member(sptr, "reserved2", BADADDR, byte_flag(), NULL, 1);
    add_struc_member(sptr, "nidaltsets", BADADDR, byte_flag(), NULL, 1);
    
    sptr = get_struc(add_struc(BADADDR, "_scelibent_ppu32"));
    if ( sptr != NULL ) {
      opinfo_t mt;
      mt.tid = libEntCommon;
      add_struc_member(sptr, "c", BADADDR, stru_flag(), &mt, get_struc_size(mt.tid));
      
      add_struc_member(sptr, "libname", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "nidtable", BADADDR, off_flag() | dword_flag(), &ot, 4);
      add_struc_member(sptr, "addtable", BADADDR, off_flag() | dword_flag(), &ot, 4);
    }
  }
  
  tid_t procParamInfo = add_struc(BADADDR, "sys_process_param_t");
  sptr = get_struc(procParamInfo);
  if ( sptr != NULL ) {
    add_struc_member(sptr, "size", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "magic", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "version", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "sdk_version", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "primary_prio", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "primary_stacksize", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "malloc_pagesize", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "ppc_seg", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "crash_dump_param_addr", BADADDR, dword_flag(), NULL, 4);
  }
  
  tid_t procPrxInfo = add_struc(BADADDR, "sys_process_prx_info_t");
  sptr = get_struc(procPrxInfo);
  if ( sptr != NULL ) {
    add_struc_member(sptr, "size", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "magic", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "version", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "sdk_version", BADADDR, dword_flag(), NULL, 4);
    add_struc_member(sptr, "libent_start", BADADDR, off_flag() | dword_flag(), &ot, 4);
    add_struc_member(sptr, "libent_end", BADADDR, off_flag() | dword_flag(), &ot, 4);
    add_struc_member(sptr, "libstub_start", BADADDR, off_flag() | dword_flag(), &ot, 4);
    add_struc_member(sptr, "libstub_end", BADADDR, off_flag() | dword_flag(), &ot, 4);
    add_struc_member(sptr, "major_version", BADADDR, byte_flag(), NULL, 1);
    add_struc_member(sptr, "minor_version", BADADDR, byte_flag(), NULL, 1);
    add_struc_member(sptr, "reserved", BADADDR, byte_flag(), NULL, 6);
  }
}