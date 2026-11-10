using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Falcon.Generators;

// Remote procedure calls ([Rpc.Broadcast], [Rpc.Host], [Rpc.Owner] on a component's methods):
// every call to one is intercepted (C# interceptors: Falcon.Game.targets enables them for
// Falcon.Generated) by a method that writes its arguments for Falcon.Rpc to send, then runs it here
// when it runs here. Each method is also registered with its id and a dispatch that reads its
// arguments and calls it (from the network). No reflection.
[Generator]
public sealed class RpcGenerator : IIncrementalGenerator
{
    private static readonly DiagnosticDescriptor BadRpc = new(
        "FALCON004", "RPC method not usable",
        "[Rpc] {0}.{1}: {2}",
        "Falcon", DiagnosticSeverity.Warning, true);

    private static readonly string[] Attributes =
        { "Falcon.Rpc+BroadcastAttribute", "Falcon.Rpc+HostAttribute", "Falcon.Rpc+OwnerAttribute" };

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        var methods = Attributes.Select(a => context.SyntaxProvider.ForAttributeWithMetadataName(a,
                                                static (node, _) => node is MethodDeclarationSyntax,
                                                static (ctx, _) => (IMethodSymbol)ctx.TargetSymbol)
                                            .Collect())
            .ToArray();
        var calls = context.SyntaxProvider.CreateSyntaxProvider(
                static (node, _) => node is InvocationExpressionSyntax,
                static (ctx, _) =>
                {
                    var invocation = (InvocationExpressionSyntax)ctx.Node;
                    if (ctx.SemanticModel.GetSymbolInfo(invocation).Symbol is not IMethodSymbol m || RpcAttribute(m) == null)
                        return default;
                    var location = ctx.SemanticModel.GetInterceptableLocation(invocation);
                    return location == null ? default : (m.OriginalDefinition, location.GetInterceptsLocationAttributeSyntax());
                })
            .Where(static c => c.Item1 != null)
            .Collect();
        var all = methods[0].Combine(methods[1]).Combine(methods[2]).Combine(calls).Combine(context.CompilationProvider);
        context.RegisterSourceOutput(all, static (spc, x) =>
        {
            var declared = x.Left.Left.Left.Left.Concat(x.Left.Left.Left.Right).Concat(x.Left.Left.Right);
            Emit(spc, x.Right, declared.ToImmutableArray(), x.Left.Right);
        });
    }

    private static AttributeData? RpcAttribute(IMethodSymbol m) =>
        m.GetAttributes().FirstOrDefault(a =>
        {
            string? n = a.AttributeClass?.ToDisplayString();
            return n == "Falcon.Rpc.BroadcastAttribute" || n == "Falcon.Rpc.HostAttribute" || n == "Falcon.Rpc.OwnerAttribute";
        });

    private static bool IsComponent(INamedTypeSymbol? t)
    {
        for (INamedTypeSymbol? b = t?.BaseType; b != null; b = b.BaseType)
            if (b.ToDisplayString() == "Falcon.Component")
                return true;
        return false;
    }

    // An argument's kind (the [Sync] kinds), "" when it can't be sent.
    private static string KindOf(ITypeSymbol t)
    {
        if (t.TypeKind == TypeKind.Enum)
            return "enum";
        switch (t.SpecialType)
        {
        case SpecialType.System_Boolean: return "bool";
        case SpecialType.System_Byte: return "byte";
        case SpecialType.System_Int32: return "int";
        case SpecialType.System_UInt32: return "uint";
        case SpecialType.System_Int64: return "long";
        case SpecialType.System_UInt64: return "ulong";
        case SpecialType.System_Single: return "float";
        case SpecialType.System_Double: return "double";
        case SpecialType.System_String: return "string";
        }
        switch (t.ToDisplayString())
        {
        case "System.Numerics.Vector2": return "vec2";
        case "System.Numerics.Vector3": return "vec3";
        case "System.Numerics.Vector4": return "vec4";
        case "System.Numerics.Quaternion": return "quat";
        case "Falcon.Entity": return "entity";
        }
        return "";
    }

    private static string ReadOf(string kind, string type) => kind switch
    {
        "bool" => "r.Bool()",
        "byte" => "r.Byte()",
        "int" => "r.Int()",
        "uint" => "r.UInt()",
        "long" => "r.Long()",
        "ulong" => "r.ULong()",
        "float" => "r.Float()",
        "double" => "r.Double()",
        "string" => "r.String()",
        "enum" => $"({type})r.Long()",
        "vec2" => "r.Vector2()",
        "vec3" => "r.Vector3()",
        "vec4" => "r.Vector4()",
        "quat" => "r.Quaternion()",
        _ => "r.Entity()",
    };

    // The method's id on every machine: FNV-1a of its class, name and parameter types.
    private static uint Id(IMethodSymbol m)
    {
        string key = m.ContainingType.ToDisplayString() + "." + m.Name + "(" +
                     string.Join(",", m.Parameters.Select(p => p.Type.ToDisplayString())) + ")";
        uint h = 2166136261;
        foreach (char c in key)
            h = (h ^ c) * 16777619;
        return h;
    }

    private static void Emit(SourceProductionContext spc, Compilation compilation, ImmutableArray<IMethodSymbol> declared,
                             ImmutableArray<(IMethodSymbol method, string attribute)> calls)
    {
        var methods = declared.Distinct(SymbolEqualityComparer.Default).Cast<IMethodSymbol>().OrderBy(m => m.ToDisplayString()).ToList();
        if (methods.Count == 0)
            return;
        var usable = new List<IMethodSymbol>();
        foreach (IMethodSymbol m in methods)
        {
            string? why = m.IsStatic ? "it must be an instance method"
                        : !IsComponent(m.ContainingType) ? "its class must be a Falcon.Component"
                        : !m.ReturnsVoid ? "it must return void (it runs elsewhere too)"
                        : m.IsGenericMethod ? "it can't be generic"
                        : m.DeclaredAccessibility == Accessibility.Private || m.DeclaredAccessibility == Accessibility.Protected
                            ? "it must not be private or protected (the generated code calls it)"
                        : m.Parameters.FirstOrDefault(p => p.RefKind != RefKind.None) is IParameterSymbol r ? $"{r.Name} can't be ref, in or out"
                        : m.Parameters.FirstOrDefault(p => KindOf(p.Type) == "") is IParameterSymbol bad
                            ? $"{bad.Name} is a {bad.Type.ToDisplayString()}, which can't be sent"
                            : null;
            if (why != null)
                spc.ReportDiagnostic(Diagnostic.Create(BadRpc, m.Locations.FirstOrDefault(), m.ContainingType.Name, m.Name, why));
            else
                usable.Add(m);
        }
        var sb = new StringBuilder();
        sb.AppendLine("// <auto-generated/> Falcon.Engine.Generators: the game's RPCs.");
        sb.AppendLine("#nullable enable");
        sb.AppendLine("namespace System.Runtime.CompilerServices");
        sb.AppendLine("{");
        sb.AppendLine("    [global::System.AttributeUsage(global::System.AttributeTargets.Method, AllowMultiple = true)]");
        sb.AppendLine("    file sealed class InterceptsLocationAttribute : global::System.Attribute");
        sb.AppendLine("    {");
        sb.AppendLine("        public InterceptsLocationAttribute(int version, string data) { _ = version; _ = data; }");
        sb.AppendLine("    }");
        sb.AppendLine("}");
        sb.AppendLine("namespace Falcon.Generated");
        sb.AppendLine("{");
        string id = new string((compilation.AssemblyName ?? "Game").Select(ch => char.IsLetterOrDigit(ch) ? ch : '_').ToArray());
        sb.AppendLine($"    internal static class Rpcs_{id}");
        sb.AppendLine("    {");
        sb.AppendLine("        [global::System.Runtime.CompilerServices.ModuleInitializer]");
        sb.AppendLine("        internal static void Register()");
        sb.AppendLine("        {");
        foreach (IMethodSymbol m in usable)
        {
            string type = m.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
            string args = string.Join(", ", m.Parameters.Select(p => ReadOf(KindOf(p.Type), p.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat))));
            sb.AppendLine($"            global::Falcon.Rpc.Register({Id(m)}u, {Kind(m)}, typeof({type}), " +
                          $"static (global::Falcon.Component c, ref global::Falcon.SyncReader r) => (({type})c).{m.Name}({args}));");
        }
        sb.AppendLine("        }");
        int n = 0;
        foreach (IMethodSymbol m in usable)
        {
            var sites = calls.Where(c => SymbolEqualityComparer.Default.Equals(c.method, m)).Select(c => c.attribute).Distinct().ToList();
            if (sites.Count == 0)
                continue;
            string type = m.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
            AttributeData attr = RpcAttribute(m)!;
            bool reliable = attr.NamedArguments.Where(a => a.Key == "Reliable").Select(a => a.Value.Value is bool b ? b : true).DefaultIfEmpty(true).First();
            foreach (string site in sites)
                sb.AppendLine("        " + site);
            string parameters = string.Concat(m.Parameters.Select(p => $", {p.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)} {p.Name}"));
            sb.AppendLine($"        internal static void {m.Name}_{n++}(this {type} self{parameters})");
            sb.AppendLine("        {");
            sb.AppendLine($"            global::Falcon.SyncWriter? w = global::Falcon.Rpc.Begin(self, {Id(m)}u, {Kind(m)}, {(reliable ? "true" : "false")});");
            sb.AppendLine("            if (w != null)");
            sb.AppendLine("            {");
            foreach (IParameterSymbol p in m.Parameters)
                sb.AppendLine(KindOf(p.Type) == "enum" ? $"                w.Write((long){p.Name});" : $"                w.Write({p.Name});");
            sb.AppendLine("                global::Falcon.Rpc.End();");
            sb.AppendLine("            }");
            sb.AppendLine($"            if (global::Falcon.Rpc.RunsHere(self, {Kind(m)}))");
            sb.AppendLine($"                self.{m.Name}({string.Join(", ", m.Parameters.Select(p => p.Name))});");
            sb.AppendLine("        }");
        }
        sb.AppendLine("    }");
        sb.AppendLine("}");
        spc.AddSource("Rpcs.g.cs", sb.ToString());
    }

    private static int Kind(IMethodSymbol m)
    {
        AttributeData a = RpcAttribute(m)!;
        string n = a.AttributeClass!.ToDisplayString();
        int kind = n == "Falcon.Rpc.HostAttribute" ? 1 : n == "Falcon.Rpc.OwnerAttribute" ? 2 : 0;
        bool any = a.NamedArguments.Any(x => x.Key == "AnyCaller" && x.Value.Value is true);
        return kind | (any ? 4 : 0);
    }
}
