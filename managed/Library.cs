using System;
using System.Runtime.InteropServices;

public static class StringDeduper
{
    public static void Initialize()
    {
        CreateCLRProfiling(out var instance);
        if (InitializeStringDeduper(@"StringDedupingProfiler.dll", typeof(string).TypeHandle.Value, instance) != 0)
        {
            throw new Exception("String deduping initialization failed. Currently works on WinX64 only. This library uses the Profiling API so ensure no other profiler is attached.");
        }
    }

    [DllImport("coreclr.dll")]
    private static extern int CreateCLRProfiling(out IntPtr instance);

    [DllImport(@"StringDedupingProfiler.dll")]
    private static extern int InitializeStringDeduper([MarshalAs(UnmanagedType.LPWStr)] string profilerPath, IntPtr stringTypeHandle, IntPtr instance);
}