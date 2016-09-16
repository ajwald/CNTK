#include "stdafx.h"
#include "Basics.h"
#include "GPUDataTransferer.h"
#include "GPUMatrix.h"

#pragma comment(lib, "cudart.lib")

#pragma warning(disable : 4267) // conversion from 'size_t' to 'unsigned int'; happens in CUDA <<<a,b>>> syntax if a and b are size_t
#pragma warning(disable : 4127) // conditional expression is constant; "if (sizeof(ElemType)==sizeof(float))" triggers this
#pragma warning(disable : 4702) // unreachable code; triggered for unknown reasons

namespace Microsoft { namespace MSR { namespace CNTK {

// CUDA failed
// Since the outer code sometimes does not recover properly, as an option we log and die right away.
// This is needed for our GCD farm which has intermittent CUDA errors that sometimes cause the DBN tool, when running with MPI, to hang instead of terminating.
static void cudafail(const char* msg)
{
    // TODO: get from an env variable
    bool dieoncudafailure = false;
    if (!dieoncudafailure)
    {
        RuntimeError("%s", msg);
    }
    fprintf(stderr, "%s\n", msg);
    fprintf(stderr, "cudafail: terminating\n"), fflush(stderr);
#ifdef WIN32
    TerminateProcess(GetCurrentProcess(), EXIT_FAILURE); // fail the hard way to ensure it won't hang elsewhere
#else
    exit(1);
#endif
}

// allows to write cudaFunction() || "error"   (CUDA runtime)
static
#ifdef WIN32
    __declspec(noinline)
#endif
        void
        operator||(cudaError_t rc, const char* msg)
{
    if (rc != cudaSuccess)
    {
        char buf[1000];
        sprintf_s(buf, 1000, "%s: %s (cuda error %d)", msg, cudaGetErrorString(rc), rc);
        cudafail(buf);
    }
}

//// Base class for different data transferers.

GranularGPUDataTransferer::GranularGPUDataTransferer(int deviceId, bool blocking /*= false*/)
    : m_fetchStream(nullptr),
      m_assignStream(nullptr),
      m_deviceId(deviceId)
{
    PrepareDevice(m_deviceId);

    // Note: Do NOT use cudaEventBlockingSync (which supposedly yields the process)--it will totally break cudaEventSynchronize(), causing it to take 50 or 100 ms randomly.
    // NOTE: We never saw this in reading prefetch.
    unsigned flags = cudaEventDisableTiming;
    if (blocking)
        flags |= cudaEventBlockingSync;

    // events
    cudaEventCreateWithFlags(&m_fetchCompleteEvent, flags) || "cudaEventCreateWithFlags failed";
    cudaEventCreateWithFlags(&m_assignCompleteEvent, flags) || "cudaEventCreateWithFlags failed";
}

GranularGPUDataTransferer::~GranularGPUDataTransferer()
{
    // TODO: Check for error code and throw if !std::uncaught_exception()
    cudaEventDestroy(m_assignCompleteEvent);
    cudaEventDestroy(m_fetchCompleteEvent);
}

void GranularGPUDataTransferer::CopyGPUToCPUAsync(const void* gpuBuffer, size_t numElements, size_t elementSize, void* cpuBuffer)
{
    PrepareDevice(m_deviceId);

    cudaMemcpyAsync(cpuBuffer, gpuBuffer, numElements * elementSize, cudaMemcpyDeviceToHost, m_fetchStream) || "cudaMemcpyAsync failed";
}

void GranularGPUDataTransferer::RecordGPUToCPUCopy()
{
    cudaEventRecord(m_fetchCompleteEvent, m_fetchStream) || "cudaEventRecord failed";
}

void GranularGPUDataTransferer::WaitForCopyGPUToCPU()
{
    PrepareDevice(m_deviceId);
    cudaEventSynchronize(m_fetchCompleteEvent) || "cudaEventSynchronize failed";
}

void GranularGPUDataTransferer::CopyCPUToGPUAsync(const void* cpuBuffer, size_t numElements, size_t elementSize, void* gpuBuffer)
{
    PrepareDevice(m_deviceId);
    cudaMemcpyAsync(gpuBuffer, cpuBuffer, numElements * elementSize, cudaMemcpyHostToDevice, m_assignStream) || "cudaMemcpyAsync failed";
}

void GranularGPUDataTransferer::RecordCPUToGPUCopy()
{
    cudaEventRecord(m_assignCompleteEvent, m_assignStream) || "cudaEventRecord failed";
}

void GranularGPUDataTransferer::WaitForCopyCPUToGPU()
{
    PrepareDevice(m_deviceId);
    cudaEventSynchronize(m_assignCompleteEvent) || "cudaEventSynchronize failed";
}

//// GPUDataTransferer

// same but for event
template <class ElemType>
void GPUDataTransferer<ElemType>::SyncEvent(cudaEvent_t ev)
{
    auto rc = cudaEventQuery(ev);
    if (rc != cudaErrorNotReady)
    {
        // if Event is ready then no need to wait
        rc || "cudaEventQuery failed";
        return;
    }
    // we must wait
    cudaEventSynchronize(ev) || "cudaEventSynchronize failed";
}

//streams
template <class ElemType>
cudaStream_t GPUDataTransferer<ElemType>::s_fetchStream = NULL;

template <class ElemType>
cudaStream_t GPUDataTransferer<ElemType>::s_assignStream = NULL;

template <class ElemType>
cudaStream_t GPUDataTransferer<ElemType>::GetFetchStream()
{
    return s_fetchStream;
}

template <class ElemType>
GPUDataTransferer<ElemType>::GPUDataTransferer(int deviceId, bool useConcurrentStreams) 
    : m_inner(new GranularGPUDataTransferer(deviceId))
{
#pragma warning(disable : 4127)
    if (useConcurrentStreams && (s_fetchStream == NULL))
    {
        cudaStreamCreateWithFlags(&s_fetchStream, cudaStreamNonBlocking) || "cudaStreamCreateWithFlags failed";
        cudaStreamCreateWithFlags(&s_assignStream, cudaStreamNonBlocking) || "cudaStreamCreateWithFlags failed";
    }

    m_inner->m_fetchStream = s_fetchStream;
    m_inner->m_assignStream = s_assignStream;
}

template <class ElemType>
GPUDataTransferer<ElemType>::~GPUDataTransferer()
{
    delete m_inner;
    // BUGBUG: we don't destroy our streams (they are static variables); we need a static destructor, I am too lazy now
}

template <class ElemType>
void GPUDataTransferer<ElemType>::CopyGPUToCPUAsync(ElemType* gpuBuffer, size_t numElements, ElemType* cpuBuffer)
{
    m_inner->CopyGPUToCPUAsync(gpuBuffer, numElements, sizeof(ElemType), cpuBuffer);
    m_inner->RecordGPUToCPUCopy();
}

template <class ElemType>
void GPUDataTransferer<ElemType>::CopyCPUToGPUAsync(ElemType* cpuBuffer, size_t numElements, ElemType* gpuBuffer)
{
    m_inner->CopyCPUToGPUAsync(cpuBuffer, numElements, sizeof(ElemType), gpuBuffer);
    m_inner->RecordCPUToGPUCopy();
}

template <class ElemType>
void GPUDataTransferer<ElemType>::WaitForCopyGPUToCPUAsync()
{
    PrepareDevice(m_inner->m_deviceId);

    SyncEvent(m_inner->m_fetchCompleteEvent);
}

template <class ElemType>
void GPUDataTransferer<ElemType>::WaitForCopyCPUToGPUAsync()
{
    PrepareDevice(m_inner->m_deviceId);

    SyncEvent(m_inner->m_assignCompleteEvent);
}

//explicit
template class GPUDataTransferer<float>;
template class GPUDataTransferer<double>;

/// PrefetchGPUDataTransferer

cudaStream_t PrefetchGPUDataTransferer::s_prefetchStream = nullptr;

PrefetchGPUDataTransferer::PrefetchGPUDataTransferer(int deviceId) : GranularGPUDataTransferer(deviceId, true)
{
#pragma warning(disable : 4127)
    if (s_prefetchStream == nullptr)
    {
        // TODO: Currently the s_prefetchStream is not destroyed.
        // As static it can be used in several readers with different lifecycle so we allow it to live till the end.
        cudaStreamCreateWithFlags(&s_prefetchStream, cudaStreamNonBlocking) || "cudaStreamCreateWithFlags failed";
    }

    m_assignStream = s_prefetchStream;
}

} } }
