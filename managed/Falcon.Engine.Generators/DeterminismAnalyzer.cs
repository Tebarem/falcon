using System.Collections.Immutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Operations;

namespace Falcon.Generators;

// FALCON100: what gives other results on another PC or run, used where the simulation runs (a
// component's or system's OnFixedUpdate, a component's collision and trigger callbacks). Replays
// and the network re-run those ticks and must get the same bits: MathF / Math transcendentals
// call the C runtime (CPU- and Windows-version dependent), System.Random and the clocks differ
// between runs. Each has a deterministic replacement in Falcon (DMath, Sim.Random, Time.Tick).
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class DeterminismAnalyzer : DiagnosticAnalyzer
{
    private static readonly DiagnosticDescriptor NotDeterministic = new(
        "FALCON100", "Not deterministic in the simulation",
        "{0} can give other results on another PC or run; in {1} (the simulation's ticks) use {2}",
        "Falcon", DiagnosticSeverity.Warning, isEnabledByDefault: true);

    private static readonly ImmutableHashSet<string> TickMethods =
        ImmutableHashSet.Create("OnFixedUpdate", "OnCollisionEnter", "OnTriggerEnter", "OnTriggerExit", "OnSimulate");

    private static readonly ImmutableHashSet<string> Transcendentals = ImmutableHashSet.Create(
        "Sin", "Cos", "Tan", "Asin", "Acos", "Atan", "Atan2", "SinCos", "Sinh", "Cosh", "Tanh", "Asinh", "Acosh", "Atanh",
        "Exp", "Log", "Log2", "Log10", "Pow", "Cbrt");

    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics => ImmutableArray.Create(NotDeterministic);

    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterOperationAction(Invocation, OperationKind.Invocation);
        context.RegisterOperationAction(PropertyReference, OperationKind.PropertyReference);
        context.RegisterOperationAction(ObjectCreation, OperationKind.ObjectCreation);
    }

    // The tick method the operation is in (null: not in one of a Component's / GameSystem's).
    private static string? TickMethod(OperationAnalysisContext context)
    {
        if (context.ContainingSymbol is not IMethodSymbol method || !method.IsOverride || !TickMethods.Contains(method.Name))
            return null;
        for (INamedTypeSymbol? t = method.ContainingType; t != null; t = t.BaseType)
            if (t.ContainingNamespace?.ToDisplayString() == "Falcon" && (t.Name == "Component" || t.Name == "GameSystem"))
                return method.Name;
        return null;
    }

    private static void Report(OperationAnalysisContext context, string what, string tick, string instead) =>
        context.ReportDiagnostic(Diagnostic.Create(NotDeterministic, context.Operation.Syntax.GetLocation(), what, tick, instead));

    private static void Invocation(OperationAnalysisContext context)
    {
        var call = (IInvocationOperation)context.Operation;
        IMethodSymbol m = call.TargetMethod;
        string type = m.ContainingType.ToDisplayString();
        if ((type == "System.MathF" || type == "System.Math") && Transcendentals.Contains(m.Name))
        {
            if (TickMethod(context) is string tick)
                Report(context, $"{m.ContainingType.Name}.{m.Name}", tick, $"DMath.{m.Name}");
        }
        else if (type == "System.Numerics.Quaternion" && (m.Name == "Slerp" || m.Name == "CreateFromAxisAngle" || m.Name == "CreateFromYawPitchRoll"))
        {
            if (TickMethod(context) is string tick)
                Report(context, $"Quaternion.{m.Name}", tick,
                       m.Name == "Slerp" ? "DMath.Slerp" : m.Name == "CreateFromAxisAngle" ? "DMath.AxisAngle" : "DMath.YawPitchRoll");
        }
        else if (type == "System.Random" || type == "System.Diagnostics.Stopwatch")
        {
            if (TickMethod(context) is string tick)
                Report(context, $"{m.ContainingType.Name}.{m.Name}", tick, type == "System.Random" ? "Sim.Random" : "Time.Tick");
        }
    }

    private static void PropertyReference(OperationAnalysisContext context)
    {
        var p = (IPropertyReferenceOperation)context.Operation;
        string type = p.Property.ContainingType.ToDisplayString();
        string? instead = (type, p.Property.Name) switch
        {
            ("System.DateTime", "Now") or ("System.DateTime", "UtcNow") or ("System.DateTimeOffset", "Now") or
                ("System.DateTimeOffset", "UtcNow") => "Time.Tick",
            ("System.Environment", "TickCount") or ("System.Environment", "TickCount64") => "Time.Tick",
            ("System.Random", "Shared") => "Sim.Random",
            _ => null,
        };
        if (instead != null && TickMethod(context) is string tick)
            Report(context, $"{p.Property.ContainingType.Name}.{p.Property.Name}", tick, instead);
    }

    private static void ObjectCreation(OperationAnalysisContext context)
    {
        var o = (IObjectCreationOperation)context.Operation;
        if (o.Type?.ToDisplayString() == "System.Random" && TickMethod(context) is string tick)
            Report(context, "new Random()", tick, "Sim.Random");
    }
}
