#include "sim.h"
#include "common.h"
#include "load_elf.h"
#include <sys/mman.h>
#include <map>
#include <iostream>

class memory_t : public loader_t
{
public:
  memory_t(char* _mem, size_t _size) : mem(_mem), size(_size) {}

  void write(size_t addr, size_t bytes, const void* src = NULL)
  {
    demand(addr < size && addr + bytes <= size, "out of bounds!");
    if(src)
      memcpy(mem+addr, src, bytes);
    else
      memset(mem+addr, 0, bytes);
  }

private:
  char* mem;
  size_t size;
};

sim_t::sim_t(int _nprocs, size_t _memsz)
  : nprocs(_nprocs), memsz(_memsz)
{
  mem = (char*)mmap(NULL, memsz, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  demand(mem != MAP_FAILED, "couldn't allocate target machine's memory");

  procs = (processor_t*)malloc(sizeof(*procs)*nprocs);
  for(int i = 0; i < nprocs; i++)
    new(&procs[i]) processor_t(i,mem,memsz);
}

sim_t::~sim_t()
{
  free(procs);
}

void sim_t::load_elf(const char* fn)
{
  memory_t loader(mem, memsz);
  ::load_elf(fn,&loader);
}

void sim_t::run(bool debug)
{
  while(1)
  {
    if(!debug)
      step_all(100,100,false);
    else
    {
      putchar(':');
      char s[128];
      std::cin.getline(s,sizeof(s)-1);

      char* p = strtok(s," ");
      if(!p)
      {
        interactive_run_noisy(std::vector<std::string>(1,"1"));
        continue;
      }
      std::string cmd = p;

      std::vector<std::string> args;
      while((p = strtok(NULL," ")))
        args.push_back(p);


      typedef void (sim_t::*interactive_func)(const std::vector<std::string>&);
      std::map<std::string,interactive_func> funcs;

      funcs["r"] = &sim_t::interactive_run_noisy;
      funcs["rs"] = &sim_t::interactive_run_silent;
      funcs["rp"] = &sim_t::interactive_run_proc_noisy;
      funcs["rps"] = &sim_t::interactive_run_proc_silent;
      funcs["reg"] = &sim_t::interactive_reg;
      funcs["mem"] = &sim_t::interactive_mem;
      funcs["until"] = &sim_t::interactive_until;
      funcs["q"] = &sim_t::interactive_quit;

      try
      {
        if(funcs.count(cmd))
          (this->*funcs[cmd])(args);
      }
      catch(trap_t t) {}
    }
  }
}

void sim_t::step_all(size_t n, size_t interleave, bool noisy)
{
  for(size_t j = 0; j < n; j+=interleave)
    for(int i = 0; i < nprocs; i++)
      procs[i].step(interleave,noisy);
}

void sim_t::interactive_run_noisy(const std::vector<std::string>& args)
{
  interactive_run(args,true);
}

void sim_t::interactive_run_silent(const std::vector<std::string>& args)
{
  interactive_run(args,false);
}

void sim_t::interactive_run(const std::vector<std::string>& args, bool noisy)
{
  if(args.size())
    step_all(atoi(args[0].c_str()),1,noisy);
  else
    while(1) step_all(1,1,noisy);
}

void sim_t::interactive_run_proc_noisy(const std::vector<std::string>& args)
{
  interactive_run_proc(args,true);
}

void sim_t::interactive_run_proc_silent(const std::vector<std::string>& args)
{
  interactive_run_proc(args,false);
}

void sim_t::interactive_run_proc(const std::vector<std::string>& a, bool noisy)
{
  if(a.size() == 0)
    return;

  int p = atoi(a[0].c_str());
  if(p >= nprocs)
    return;

  if(a.size() == 2)
    procs[p].step(atoi(a[1].c_str()),noisy);
  else
    while(1) procs[p].step(1,noisy);
}

void sim_t::interactive_quit(const std::vector<std::string>& args)
{
  exit(0);
}

reg_t sim_t::get_pc(const std::vector<std::string>& args)
{
  if(args.size() != 1)
    throw trap_illegal_instruction;

  int p = atoi(args[0].c_str());
  if(p >= nprocs)
    throw trap_illegal_instruction;

  return procs[p].pc;
}

reg_t sim_t::get_reg(const std::vector<std::string>& args)
{
  if(args.size() != 2)
    throw trap_illegal_instruction;

  int p = atoi(args[0].c_str());
  int r = atoi(args[1].c_str());
  if(p >= nprocs || r >= NGPR)
    throw trap_illegal_instruction;

  return procs[p].R[r];
}

void sim_t::interactive_reg(const std::vector<std::string>& args)
{
  printf("0x%016llx\n",(unsigned long long)get_reg(args));
}

reg_t sim_t::get_mem(const std::vector<std::string>& args)
{
  if(args.size() != 1)
    throw trap_illegal_instruction;

  reg_t addr = strtol(args[0].c_str(),NULL,16), val;
  mmu_t mmu(mem,memsz);
  switch(addr % 8)
  {
    case 0:
      val = mmu.load_uint64(addr);
      break;
    case 4:
      val = mmu.load_uint32(addr);
      break;
    case 2:
    case 6:
      val = mmu.load_uint16(addr);
      break;
    default:
      val = mmu.load_uint8(addr);
      break;
  }
  return val;
}

void sim_t::interactive_mem(const std::vector<std::string>& args)
{
  printf("0x%016llx\n",(unsigned long long)get_mem(args));
}

void sim_t::interactive_until(const std::vector<std::string>& args)
{
  if(args.size() < 3)
    return;

  std::string cmd = args[0];
  reg_t val = strtol(args[args.size()-1].c_str(),NULL,16);
  
  std::vector<std::string> args2;
  args2 = std::vector<std::string>(args.begin()+1,args.end()-1);

  while(1)
  {
    reg_t current;
    if(args[0] == "reg")
      current = get_reg(args2);
    else if(args[0] == "pc")
      current = get_pc(args2);
    else if(args[0] == "mem")
      current = get_mem(args2);
    else
      return;

    if(current == val)
      break;

    step_all(1,1,false);
  }
}