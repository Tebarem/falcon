// Linked into every game project (Directory.Build.targets, FalconGame=true); not part of
// Falcon.Engine itself. The NativeAOT export lives in the game's own module: calling it is the
// first access to that module, which runs the game's [ModuleInitializer] (Game.Register)
// before Falcon takes over. CoreCLR (JIT) hosting does not use it.

using System.Runtime.InteropServices;
using Falcon.Native;

namespace Falcon.Generated;

internal static unsafe class AotEntry
{
    [UnmanagedCallersOnly(EntryPoint = "falcon_script_init")]
    private static int Init(FxNativeApi* native, FxManagedApi* managed, byte* gamePath) =>
        Host.InitializeLinked(native, managed);
}
