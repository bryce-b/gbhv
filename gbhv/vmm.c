#include "vmm.h"
#include "vmx.h"
#include "vmcs.h"
/**
 * Call HvInitializeLogicalProcessor on all processors using an Inter-Process Interrupt (IPI).
 * 
 * All processors will stop executing until all processors have entered VMX root-mode.
 */
BOOL HvInitializeAllProcessors()
{
	SIZE_T FeatureMSR;
	PVMM_GLOBAL_CONTEXT GlobalContext;

	HvUtilLog("HvInitializeAllProcessors: Starting.");

	// Check if VMX support is enabled on the processor.
	if (!ArchIsVMXAvailable())
	{
		HvUtilLogError("VMX is not a feture of this processor.");
		return FALSE;
	}

	// Enable bits in MSR to enable VMXON instruction.
	FeatureMSR = ArchGetHostMSR(MSR_IA32_FEATURE_CONTROL_ADDRESS);

	// The BIOS will lock the VMX bit on startup.
	if(!HvUtilBitIsSet(FeatureMSR, FEATURE_BIT_VMX_LOCK))
	{
		HvUtilLogError("VMX support was not locked by BIOS.");
		return FALSE;
	}

	// VMX support can be configured to be disabled outside SMX.
	// Check to ensure this isn't the case.
	if (!HvUtilBitIsSet(FeatureMSR, FEATURE_BIT_ALLOW_VMX_OUTSIDE_SMX))
	{
		HvUtilLogError("VMX support was disabled outside of SMX operation by BIOS.");
		return FALSE;
	}

	HvUtilLog("Total Processor Count: %i", OsGetCPUCount());

	// Pre-allocate all logical processor contexts, VMXON regions, VMCS regions
	GlobalContext = HvAllocateVmmContext();

	// Generates a DPC that makes all processors execute the broadcast function.
	KeGenericCallDpc(HvpDPCBroadcastFunction, (PVOID)GlobalContext);

	if(GlobalContext->SuccessfulInitializationsCount != OsGetCPUCount())
	{
		// TODO: Move to driver uninitalization
		HvFreeVmmContext(GlobalContext);
		HvUtilLogError("HvInitializeAllProcessors: Not all processors initialized. [%i successful]", GlobalContext->SuccessfulInitializationsCount);
		return FALSE;
	}

	HvUtilLogSuccess("HvInitializeAllProcessors: Success.");
	return TRUE;
}

/*
 * Allocate the global VMM context used by all processors.
 * 
 * Allocates a logical processor context structure for each logical processor on the system.
 * 
 * Accesses capability MSRs to get information about the VMX execution environment.
 */
PVMM_GLOBAL_CONTEXT HvAllocateVmmContext()
{
	PVMM_GLOBAL_CONTEXT Context;

	// Allocate the global context structure
	Context = (PVMM_GLOBAL_CONTEXT)OsAllocateNonpagedMemory(sizeof(VMM_CONTEXT));
	if(!Context)
	{
		return NULL;
	}

	// Get count of all logical processors on the system
	Context->ProcessorCount = OsGetCPUCount();

	// Number of successful processor initializations
	Context->SuccessfulInitializationsCount = 0;

	// Save SYSTEM process DTB
	Context->SystemDirectoryTableBase = __readcr3();

	/*
	 * Get capability MSRs and add them to the global context.
	 */
	Context->VmxCapabilities = ArchGetBasicVmxCapabilities();

	PVMM_PROCESSOR_CONTEXT* ProcessorContexts = OsAllocateNonpagedMemory(Context->ProcessorCount * sizeof(PVMM_PROCESSOR_CONTEXT));
	if(!ProcessorContexts)
	{
		return NULL;
	}

	/*
	 * Allocate a logical processor context structure for each processor on the system.
	 */
	for (SIZE_T ProcessorNumber = 0; ProcessorNumber < Context->ProcessorCount; ProcessorNumber++)
	{
		ProcessorContexts[ProcessorNumber] = HvAllocateLogicalProcessorContext(Context);
		if (ProcessorContexts[ProcessorNumber] == NULL)
		{
			HvUtilLogError("HvInitializeLogicalProcessor[#%i]: Failed to setup processor context.", ProcessorNumber);
			return NULL;
		}

		HvUtilLog("HvInitializeLogicalProcessor[#%i]: Allocated Context [Context = 0x%llx]", ProcessorNumber, ProcessorContexts[ProcessorNumber]);
	}

	Context->AllProcessorContexts = ProcessorContexts;
	HvUtilLog("VmcsRevisionNumber: %x", Context->VmxCapabilities.VmcsRevisionId);

	return Context;
}

/*
 * Free global VMM context and all logical processor contexts.
 */
VOID HvFreeVmmContext(PVMM_GLOBAL_CONTEXT Context)
{
	if(Context)
	{
		// Free each logical processor context
		for(SIZE_T ProcessorNumber = 0; ProcessorNumber < Context->ProcessorCount; ProcessorNumber++)
		{
			HvFreeLogicalProcessorContext(Context->AllProcessorContexts[ProcessorNumber]);
		}

		// Free the collection of pointers to processor contexts
		OsFreeNonpagedMemory(Context->AllProcessorContexts);

		// Free the actual context struct
		OsFreeNonpagedMemory(Context);
	}
}

/*
 * Allocate and setup the VMXON region for the logical processor context.
 */
PVMXON_REGION HvAllocateVmxonRegion(PVMM_GLOBAL_CONTEXT GlobalContext)
{
	PVMXON_REGION Region;

	// See VMX_VMXON_NUMBER_PAGES documentation.
	Region = (PVMXON_REGION)OsAllocateContiguousAlignedPages(VMX_VMXON_NUMBER_PAGES);

	// Zero VMXON region just to be sure...
	OsZeroMemory(Region, VMX_VMXON_NUMBER_PAGES * PAGE_SIZE);

	/*
	 * Initialize the version identifier in the VMXON region (the first 31 bits) with the VMCS revision identifier
	 * reported by capability MSRs.
	 *
	 * Clear bit 31 of the first 4 bytes of the VMXON region. (Handled by OsZeroMemory above)
	 */

	Region->VmcsRevisionNumber = (UINT32)GlobalContext->VmxCapabilities.VmcsRevisionId;

	return Region;
}

/**
 * Allocates a logical processor context.
 * 
 * Returns NULL on error.
 */
PVMM_PROCESSOR_CONTEXT HvAllocateLogicalProcessorContext(PVMM_GLOBAL_CONTEXT GlobalContext)
{
	PVMM_PROCESSOR_CONTEXT Context;

	// Allocate some generic memory for our context
	Context = (PVMM_PROCESSOR_CONTEXT)OsAllocateNonpagedMemory(sizeof(VMX_PROCESSOR_CONTEXT));
	if(!Context)
	{
		return NULL;
	}

	// Inititalize all fields to 0
	OsZeroMemory(Context, sizeof(VMX_PROCESSOR_CONTEXT));

	// Entry to refer back to the global context for simplicity
	Context->GlobalContext = GlobalContext;

	// Allocate and setup the VMXON region for this processor
	Context->VmxonRegion = HvAllocateVmxonRegion(GlobalContext);
	if(!Context->VmxonRegion)
	{
		return NULL;
	}

	Context->VmxonRegionPhysical = OsVirtualToPhysical(Context->VmxonRegion);
	if (!Context->VmxonRegionPhysical)
	{
		return NULL;
	}

	// Allocate and setup a blank VMCS region
	Context->VmcsRegion = HvAllocateVmcsRegion(GlobalContext);
	if(!Context->VmcsRegion)
	{
		return NULL;
	}

	Context->VmcsRegionPhysical = OsVirtualToPhysical(Context->VmcsRegion);
	if(!Context->VmcsRegionPhysical)
	{
		return NULL;
	}

	/*
	 * Allocate one page for MSR bitmap, all zeroes because we are not exiting on any MSRs.
	 */
	Context->MsrBitmap = OsAllocateContiguousAlignedPages(1);
	OsZeroMemory(Context->MsrBitmap, PAGE_SIZE);

	// Record the physical address of the MSR bitmap
	Context->MsrBitmapPhysical = OsVirtualToPhysical(Context->MsrBitmap);

	return Context;
}

/*
 * Allocate a VMCS memory region and write the revision identifier.
 */
PVMCS HvAllocateVmcsRegion(PVMM_GLOBAL_CONTEXT GlobalContext)
{
	PVMCS VmcsRegion;

	// Allocate contiguous physical pages for the VMCS. See VMX_VMCS_NUMBER_PAGES.
	VmcsRegion = (PVMCS)OsAllocateContiguousAlignedPages(VMX_VMCS_NUMBER_PAGES);

	// Initialize all fields to zero.
	OsZeroMemory(VmcsRegion, VMX_VMCS_NUMBER_PAGES * PAGE_SIZE);

	/*
	 * Software should write the VMCS revision identifier to the VMCS region before using that region for a VMCS.
	 */
	VmcsRegion->RevisionId = (UINT32)GlobalContext->VmxCapabilities.VmcsRevisionId;

	return VmcsRegion;
}

/*
 * Free a logical processor context allocated by HvAllocateLogicalProcessorContext
 */
VOID HvFreeLogicalProcessorContext(PVMM_PROCESSOR_CONTEXT Context)
{
	if(Context)
	{
		OsFreeContiguousAlignedPages(Context->VmxonRegion);
		OsFreeNonpagedMemory(Context);
	}
}

/**
 * Called by HvInitializeAllProcessors to initialize VMX on all processors.
 */
VOID NTAPI HvpDPCBroadcastFunction(_In_ struct _KDPC *Dpc,
	_In_opt_ PVOID DeferredContext,
	_In_opt_ PVOID SystemArgument1,
	_In_opt_ PVOID SystemArgument2)
{
	SIZE_T CurrentProcessorNumber;
	PVMM_GLOBAL_CONTEXT GlobalContext;
	PVMM_PROCESSOR_CONTEXT CurrentContext;

	UNREFERENCED_PARAMETER(Dpc);

	GlobalContext = (PVMM_GLOBAL_CONTEXT)DeferredContext;

	// Get the current processor number we're executing this function on right now
	CurrentProcessorNumber = OsGetCurrentProcessorNumber();

	// Get the logical processor context that was allocated for this current processor
	CurrentContext = GlobalContext->AllProcessorContexts[CurrentProcessorNumber];

	// Initialize processor for VMX
	if(HvBeginInitializeLogicalProcessor(CurrentContext))
	{
		// We were successful in initializing the processor
		GlobalContext->SuccessfulInitializationsCount++;
	}
	else
	{
		HvUtilLogError("HvpDPCBroadcastFunction[#%i]: Failed to VMLAUNCH.", CurrentProcessorNumber);
	}

	// These must be called for GenericDpcCall to signal other processors
	// SimpleVisor code shows this

	// Wait for all DPCs to synchronize at this point
	KeSignalCallDpcSynchronize(SystemArgument2);

	// Mark the DPC as being complete
	KeSignalCallDpcDone(SystemArgument1);
}

VOID __HALT_DEBUG();

/**
 * Initialize VMCS and enter VMX root-mode.
 * 
 * This function should never return, except on error. Execution will continue on the guest on success.
 * 
 * See: HvBeginInitializeLogicalProcessor and vmxdefs.asm.
 */
VOID HvInitializeLogicalProcessor(PVMM_PROCESSOR_CONTEXT Context, SIZE_T GuestRSP, SIZE_T GuestRIP)
{
	SIZE_T CurrentProcessorNumber;

	// Get the current processor we're executing this function on right now
	CurrentProcessorNumber = OsGetCurrentProcessorNumber();

	// Enable VMXe, execute VMXON and enter VMX root mode
	if (!VmxEnterRootMode(Context))
	{
		HvUtilLogError("HvInitializeLogicalProcessor[#%i]: Failed to enter VMX Root Mode.", CurrentProcessorNumber);
		return;
	}

	// Setup VMCS with all values necessary to begin VMXLAUNCH
	if (!HvSetupVmcsDefaults(Context, (SIZE_T)&HvEnterFromGuest, (SIZE_T)&Context->HostStack, GuestRIP, GuestRSP))
	{
		HvUtilLogError("HvInitializeLogicalProcessor[#%i]: Failed to enter VMX Root Mode.", CurrentProcessorNumber);
		VmxExitRootMode(Context);
		return;
	}

	// Launch the hypervisor...?
	if (!VmxLaunchProcessor(Context))
	{
		HvUtilLogError("HvInitializeLogicalProcessor[#%i]: Failed to VmxLaunchProcessor.", CurrentProcessorNumber);
		return;
	}

	HvUtilLogSuccess("HvInitializeLogicalProcessor[#%i]: Successfully entered VMX Root Mode.", CurrentProcessorNumber);

	VmxExitRootMode(Context);
}

/*
 * This function is the main handler of all exits that take place during VMM execution.
 * 
 * This function is called from HvEnterFromGuest (defined in vmxdefs.asm). During HvEnterFromGuest,
 * the state of the guest's registers are stored to the stack using inline asm, and given as a pointer in argument 1.
 * The value of most guest gp registers can be accessed using the PGPREGISTER_CONTEXT, but GuestRSP must
 * be read out of the VMCS due to the fact that, during the switch from guest to host, the HostRSP value
 * was loaded from the host area VMCS, and the guest RSP was saved back to the guest area of the VMCS.
 * 
 * Defined in Section 27.2 RECORDING VM-EXIT INFORMATION AND UPDATING VM-ENTRY CONTROL FIELDS, exits have two main
 * fields, one which describes what kind of exit just took place (ExitQualification) and the why it took place (ExitReason).
 * By reading these two values, the exit handler can know exactly how to handle the exit properly.
 * 
 * When the exit handler is called by the CPU, interrupts are disabled. In order to call certain kernel api functions in Type 2, we will need to enable interrupts.
 * Therefore, the first action the handler must take is to ensure execution of the handler is not executing below DISPATCH_LEVEL.
 * This is to prevent the dispatcher from context switching away from our exit handler if we enable interrupts, potentially causing serious memory synchronization problems.
 * 
 * The action is to read exit information and certain guest registers (RSP, RIP, RFLAGS) from the VMCS with the VMREAD instruction.
 * 
 * 
 */
BOOL HvHandleVmExit(PGPREGISTER_CONTEXT GuestRegisters)
{
	VMEXIT_CONTEXT ExitContext;

	/*
	 * Initialize all fields of the exit context, including reading relevant fields from the VMCS.
	 */
	VmxInitializeExitContext(&ExitContext, GuestRegisters);

	/*
	 * To prevent context switching while enabling interrupts, save IRQL here.
	 */
	ExitContext.SavedIRQL = KeGetCurrentIrql();
	if (ExitContext.SavedIRQL < DISPATCH_LEVEL)
	{
		KeRaiseIrqlToDpcLevel();
	}

	__debugbreak();

	/*
	 * If we raised IRQL, lower it before returning to guest.
	 */
	if(ExitContext.SavedIRQL < DISPATCH_LEVEL)
	{
		KeLowerIrql(ExitContext.SavedIRQL);
	}

	return TRUE;
}

VOID HvHandleVmExitFailure()
{
	__debugbreak();
}