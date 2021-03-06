// machine.cc 
//	Routines for simulating the execution of user programs.
//
//  DO NOT CHANGE -- part of the machine emulation
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation 
// of liability and disclaimer of warranty provisions.

#include "copyright.h"
#include "machine.h"
#include "system.h"

// Textual names of the exceptions that can be generated by user program
// execution, for debugging.
static char* exceptionNames[] = { "no exception", "syscall", 
				"page fault/no TLB entry", "page read only",
				"bus error", "address error", "overflow",
				"illegal instruction" };

//----------------------------------------------------------------------
// CheckEndian
// 	Check to be sure that the host really uses the format it says it 
//	does, for storing the bytes of an integer.  Stop on error.
//----------------------------------------------------------------------

static
void CheckEndian()
{
    union checkit {
        char charword[4];
        unsigned int intword;
    } check;

    check.charword[0] = 1;
    check.charword[1] = 2;
    check.charword[2] = 3;
    check.charword[3] = 4;

#ifdef HOST_IS_BIG_ENDIAN
    ASSERT (check.intword == 0x01020304);
#else
    ASSERT (check.intword == 0x04030201);
#endif
}

//----------------------------------------------------------------------
// Machine::Machine
// 	Initialize the simulation of user program execution.
//
//	"debug" -- if TRUE, drop into the debugger after each user instruction
//		is executed.
//----------------------------------------------------------------------

Machine::Machine(bool debug)
:bitMap(BitMap(NumPhysPages))
{
    int i;

    for (i = 0; i < NumTotalRegs; i++)
        registers[i] = 0;
    mainMemory = new char[MemorySize];
    for (i = 0; i < MemorySize; i++)
      	mainMemory[i] = 0;
#ifdef USE_TLB
    tlb = new TranslationEntry[TLBSize];
    for (i = 0; i < TLBSize; i++){
        tlb[i].valid = FALSE;
        tlb[i].hitRate = 0;
    }
    pageTable = NULL;
#else	// use linear page table
    tlb = NULL;
    pageTable = NULL;
#endif

    singleStep = debug;
    // 检查编译指定的大小端模式与实际大小端模式是否匹配
    CheckEndian();
}

//----------------------------------------------------------------------
// Machine::~Machine
// 	De-allocate the data structures used to simulate user program execution.
//----------------------------------------------------------------------

Machine::~Machine()
{
    delete [] mainMemory;
    if (tlb != NULL)
        delete [] tlb;
}

//----------------------------------------------------------------------
// Machine::RaiseException
// 	Transfer control to the Nachos kernel from user mode, because
//	the user program either invoked a system call, or some exception
//	occured (such as the address translation failed).
//
//	"which" -- the cause of the kernel trap
//	"badVaddr" -- the virtual address causing the trap, if appropriate
//----------------------------------------------------------------------

void
Machine::RaiseException(ExceptionType which, int badVAddr)
{
    DEBUG('m', "Exception: %s\n", exceptionNames[which]);
    
//  ASSERT(interrupt->getStatus() == UserMode);
    registers[BadVAddrReg] = badVAddr;
    DelayedLoad(0, 0);			// finish anything in progress
    interrupt->setStatus(SystemMode);
    ExceptionHandler(which);		// interrupts are enabled at this point
    interrupt->setStatus(UserMode);
}

//----------------------------------------------------------------------
// Machine::Debugger
// 	Primitive debugger for user programs.  Note that we can't use
//	gdb to debug user programs, since gdb doesn't run on top of Nachos.
//	It could, but you'd have to implement *a lot* more system calls
//	to get it to work!
//
//	So just allow single-stepping, and printing the contents of memory.
//----------------------------------------------------------------------

void Machine::Debugger()
{
    char *buf = new char[80];
    int num;

    interrupt->DumpState();
    DumpState();
    printf("%d> ", stats->totalTicks);
    fflush(stdout);
    fgets(buf, 80, stdin);
    if (sscanf(buf, "%d", &num) == 1)
	runUntilTime = num;
    else {
	runUntilTime = 0;
	switch (*buf) {
	  case '\n':
	    break;
	    
	  case 'c':
	    singleStep = FALSE;
	    break;
	    
	  case '?':
	    printf("Machine commands:\n");
	    printf("    <return>  execute one instruction\n");
	    printf("    <number>  run until the given timer tick\n");
	    printf("    c         run until completion\n");
	    printf("    ?         print help message\n");
	    break;
	}
    }
    delete [] buf;
}
 
//----------------------------------------------------------------------
// Machine::DumpState
// 	Print the user program's CPU state.  We might print the contents
//	of memory, but that seemed like overkill.
//----------------------------------------------------------------------

void
Machine::DumpState()
{
    int i;
    
    printf("Machine registers:\n");
    for (i = 0; i < NumGPRegs; i++)
	switch (i) {
	  case StackReg:
	    printf("\tSP(%d):\t0x%x%s", i, registers[i],
		   ((i % 4) == 3) ? "\n" : "");
	    break;
	    
	  case RetAddrReg:
	    printf("\tRA(%d):\t0x%x%s", i, registers[i],
		   ((i % 4) == 3) ? "\n" : "");
	    break;
	  
	  default:
	    printf("\t%d:\t0x%x%s", i, registers[i],
		   ((i % 4) == 3) ? "\n" : "");
	    break;
	}
    
    printf("\tHi:\t0x%x", registers[HiReg]);
    printf("\tLo:\t0x%x\n", registers[LoReg]);
    printf("\tPC:\t0x%x", registers[PCReg]);
    printf("\tNextPC:\t0x%x", registers[NextPCReg]);
    printf("\tPrevPC:\t0x%x\n", registers[PrevPCReg]);
    printf("\tLoad:\t0x%x", registers[LoadReg]);
    printf("\tLoadV:\t0x%x\n", registers[LoadValueReg]);
    printf("\n");
}

//----------------------------------------------------------------------
// Machine::ReadRegister/WriteRegister
//   	Fetch or write the contents of a user program register.
//----------------------------------------------------------------------

int Machine::ReadRegister(int num)
    {
	ASSERT((num >= 0) && (num < NumTotalRegs));
	return registers[num];
    }

void Machine::WriteRegister(int num, int value)
    {
	ASSERT((num >= 0) && (num < NumTotalRegs));
	// DEBUG('m', "WriteRegister %d, value %d\n", num, value);
	registers[num] = value;
    }

void Machine::tlbSwap(int vaddr)
{
    DEBUG('m',"Entering tlbSwap.\n");
    ASSERT(tlb);
    int swapI = -1;
    // 查找快表是否存在空项，如果存在则替换空项
    for(int i = 0; i < TLBSize; ++i){
        if(!tlb[i].valid){
            swapI = i;
            break;
        }
    }
    
    
    int vpn = (int)vaddr / PageSize;
    int offset = (int)vaddr % PageSize;
    
    // 不存在空项，执行页面置换算法
    if(swapI == -1)
    {
#ifdef TLB_LRU  // 将最近命中率最低的页表项置换出去
        int min = (1 << 31) -1;
        for(int i = 0; i < TLBSize; ++i){
            if(tlb[i].valid && (tlb[i].hitRate < min)){
                min = tlb[i].hitRate;
                swapI = i;
            }
        }
        //tlb[swapI].physicalPage = pageTable[vpn].physicalPage;//  物理页号其实不用变动，只是一个映射关系，tlb下pageTable==NULL
#else           // TLB_FIFO
        swapI = TLBSize - 1;
        for(int i = 0; i < TLBSize - 1; ++i){
            tlb[i] = tlb[i+1];
        }
        tlb[swapI].physicalPage = pageTable[vpn].physicalPage;
#endif
    }

    //printf("%d,%d,%d,%d:",vaddr,PageSize,vpn,offset);

    tlb[swapI].valid = true;
    tlb[swapI].virtualPage = vpn;
    
    tlb[swapI].use = FALSE;
    tlb[swapI].dirty = FALSE;
    tlb[swapI].readOnly = FALSE;
    tlb[swapI].hitRate = 0;

    int phyAddr = tlb[swapI].physicalPage * PageSize + offset;
    printf("tlbSwap=>vaddr:[%llx],tlb[%d].virtualPage:[%d],physicalPage:[%d],hitRate:[%d],physicalAddr:[%llx]\n",\
          vaddr,swapI,vpn,tlb[swapI].physicalPage, tlb[swapI].hitRate,phyAddr);
}

 // 从位图中寻找物理页号返回
int Machine::Find()
{
    int tmp = bitMap.Find();
    ASSERT(tmp != -1);
    printf("allocate bitmap find:[%d]\n",tmp);
    return tmp;
}

void Machine::Clear(int num)
{
    if(bitMap.Test(num)){
        bitMap.Clear(num);
        printf("[%s] deallocate bitmap:[%d]\n",currentThread->getName(),num);
    }
}

void Machine::ClearAll()
{
    bitMap.Print();

    for(int i = 0; i < pageTableSize; ++i){
#ifdef USE_TLB
        int phyPage = tlb[i].physicalPage;
#else
        int phyPage = pageTable[i].physicalPage;
#endif
        //printf("222 [%d] [%d]\n",phyPage,bitMap.Test(phyPage));
        if(bitMap.Test(phyPage)){
            bitMap.Clear(phyPage);
            printf("[%s] deallocate bitmap:[%d]\n",currentThread->getName(),phyPage);
        }
    }
}
