using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Falcon.Generators;

// Registers a game's script components (classes deriving from Falcon.Component) from a module
// initializer: a factory, each [Property]'s name, type and attributes, and code reading the
// properties from JSON (the scene's values) and writing them (defaults, hot reload, the Inspector's
// live values). No reflection: it works under NativeAOT and trimming.
[Generator]
public sealed class ComponentGenerator : IIncrementalGenerator
{
    private static readonly DiagnosticDescriptor Unsupported = new(
        "FALCON001", "Property type not supported",
        "[Property] {0}.{1} is a {2}: properties are float, double, int, uint, long, bool, string, an enum, Vector2/3/4, Quaternion or Entity",
        "Falcon", DiagnosticSeverity.Warning, true);

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        var classes = context.SyntaxProvider.CreateSyntaxProvider(
                static (node, _) => node is ClassDeclarationSyntax c && c.BaseList != null,
                static (ctx, _) => ctx.SemanticModel.GetDeclaredSymbol((ClassDeclarationSyntax)ctx.Node) as INamedTypeSymbol)
            .Where(static s => s != null && (IsComponent(s!) || IsEditorTool(s!) || IsEditorWindow(s!) || IsSystem(s!)))
            .Collect();
        // Editor code: [EditorMenu] static methods (libraries' Editor/, Scripts/Editor/).
        var menus = context.SyntaxProvider.ForAttributeWithMetadataName("Falcon.Editor.EditorMenuAttribute",
                static (node, _) => node is MethodDeclarationSyntax,
                static (ctx, _) => (IMethodSymbol)ctx.TargetSymbol)
            .Collect();
        // (API v33) Data components: the game's [Component] structs.
        var structs = context.SyntaxProvider.ForAttributeWithMetadataName("Falcon.ComponentAttribute",
                static (node, _) => node is StructDeclarationSyntax,
                static (ctx, _) => (INamedTypeSymbol)ctx.TargetSymbol)
            .Collect();
        context.RegisterSourceOutput(context.CompilationProvider.Combine(classes).Combine(menus).Combine(structs),
                                     static (spc, all) => Emit(spc, all.Left.Left.Left, all.Left.Left.Right!, all.Left.Right, all.Right));
    }

    private static bool IsEditorWindow(INamedTypeSymbol type)
    {
        if (type.IsAbstract || type.IsGenericType || type.DeclaredAccessibility == Accessibility.Private)
            return false;
        for (INamedTypeSymbol? b = type.BaseType; b != null; b = b.BaseType)
            if (b.ToDisplayString() == "Falcon.Editor.EditorWindow")
                return type.InstanceConstructors.Any(c => c.Parameters.Length == 0 && c.DeclaredAccessibility == Accessibility.Public);
        return false;
    }

    // The library a source file belongs to (".../Libraries/<name>/..."), "" for the project's own.
    private static string LibraryOf(INamedTypeSymbol type)
    {
        string path = (type.Locations.FirstOrDefault()?.SourceTree?.FilePath ?? "").Replace('\\', '/');
        int at = path.LastIndexOf("/Libraries/", System.StringComparison.OrdinalIgnoreCase);
        if (at < 0)
            return "";
        string rest = path.Substring(at + "/Libraries/".Length);
        int slash = rest.IndexOf('/');
        return slash < 0 ? "" : rest.Substring(0, slash);
    }

    private static bool IsSystem(INamedTypeSymbol type)
    {
        if (type.IsAbstract || type.IsGenericType || type.DeclaredAccessibility == Accessibility.Private)
            return false;
        for (INamedTypeSymbol? b = type.BaseType; b != null; b = b.BaseType)
            if (b.ToDisplayString() == "Falcon.GameSystem")
                return type.InstanceConstructors.Any(c => c.Parameters.Length == 0 && c.DeclaredAccessibility == Accessibility.Public);
        return false;
    }

    private static bool IsEditorTool(INamedTypeSymbol type)
    {
        if (type.IsAbstract || type.IsGenericType || type.DeclaredAccessibility == Accessibility.Private)
            return false;
        for (INamedTypeSymbol? b = type.BaseType; b != null; b = b.BaseType)
            if (b.ToDisplayString() == "Falcon.Editor.EditorTool")
                return type.InstanceConstructors.Any(c => c.Parameters.Length == 0 && c.DeclaredAccessibility == Accessibility.Public);
        return false;
    }

    private static readonly DiagnosticDescriptor BadSync = new(
        "FALCON003", "[Sync] member not usable",
        "[Sync] {0}.{1}: {2}",
        "Falcon", DiagnosticSeverity.Warning, true);

    private static readonly DiagnosticDescriptor BadMenu = new(
        "FALCON002", "Editor menu item not usable",
        "[EditorMenu] {0}.{1} must be a static method without parameters, not private",
        "Falcon", DiagnosticSeverity.Warning, true);

    // A C# modifier for level meshes (Falcon.Level.MeshModifier): registered as one, not as a component.
    private static bool IsModifier(INamedTypeSymbol type)
    {
        for (INamedTypeSymbol? b = type.BaseType; b != null; b = b.BaseType)
            if (b.ToDisplayString() == "Falcon.Level.MeshModifier")
                return true;
        return false;
    }

    private static bool IsComponent(INamedTypeSymbol type)
    {
        if (type.IsAbstract || type.IsGenericType || type.DeclaredAccessibility == Accessibility.Private)
            return false;
        for (INamedTypeSymbol? b = type.BaseType; b != null; b = b.BaseType)
            if (b.ToDisplayString() == "Falcon.Component")
                return type.InstanceConstructors.Any(c => c.Parameters.Length == 0 && c.DeclaredAccessibility == Accessibility.Public);
        return false;
    }

    private sealed class Prop
    {
        public string Name = "", Kind = "", TypeName = "";
        public string? Min, Max, Asset, Tooltip, Group;
        public string[] EnumNames = System.Array.Empty<string>();
    }

    // A data component's field kind ("": not one the editor shows).
    private static string DataKindOf(ITypeSymbol t)
    {
        if (t.TypeKind == TypeKind.Enum && t is INamedTypeSymbol e && e.EnumUnderlyingType != null)
            t = e.EnumUnderlyingType;
        switch (t.SpecialType)
        {
        case SpecialType.System_Single: return "float";
        case SpecialType.System_Double: return "double";
        case SpecialType.System_Int32: return "int";
        case SpecialType.System_UInt32: return "uint";
        case SpecialType.System_Int64: return "long";
        case SpecialType.System_UInt64: return "ulong";
        case SpecialType.System_Byte: return "byte";
        case SpecialType.System_Boolean: return "bool";
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

    private static bool IsDataComponent(INamedTypeSymbol type) =>
        type.IsUnmanagedType && !type.IsGenericType && type.DeclaredAccessibility != Accessibility.Private &&
        (type.ContainingType == null || type.ContainingType.DeclaredAccessibility != Accessibility.Private);

    private static void Emit(SourceProductionContext spc, Compilation compilation, ImmutableArray<INamedTypeSymbol?> types,
                             ImmutableArray<IMethodSymbol> menuMethods, ImmutableArray<INamedTypeSymbol> structTypes)
    {
        var all = types.Where(t => t != null).Select(t => t!).Distinct(SymbolEqualityComparer.Default).Cast<INamedTypeSymbol>()
            .OrderBy(t => t.ToDisplayString()).ToList();
        var unique = all.Where(IsComponent).ToList();
        var tools = all.Where(IsEditorTool).ToList();
        var windows = all.Where(IsEditorWindow).ToList();
        var systems = all.Where(IsSystem).ToList();
        var datas = structTypes.Distinct(SymbolEqualityComparer.Default).Cast<INamedTypeSymbol>().Where(IsDataComponent)
            .OrderBy(t => t.ToDisplayString()).ToList();
        var menus = menuMethods.Distinct(SymbolEqualityComparer.Default).Cast<IMethodSymbol>().OrderBy(m => m.ToDisplayString()).ToList();
        if (unique.Count == 0 && tools.Count == 0 && menus.Count == 0 && windows.Count == 0 && systems.Count == 0 && datas.Count == 0)
            return;
        var sb = new StringBuilder();
        sb.AppendLine("// <auto-generated/> Falcon.Engine.Generators: the game's script components.");
        sb.AppendLine("#nullable enable");
        sb.AppendLine("namespace Falcon.Generated");
        sb.AppendLine("{");
        string id = new string((compilation.AssemblyName ?? "Game").Select(ch => char.IsLetterOrDigit(ch) ? ch : '_').ToArray());
        sb.AppendLine($"    internal static class ScriptComponents_{id}");
        sb.AppendLine("    {");
        foreach (INamedTypeSymbol type in datas)
        {
            string full = type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
            sb.AppendLine($"        private static global::Falcon.DataField[] DataFields{datas.IndexOf(type)}()");
            sb.AppendLine("        {");
            sb.AppendLine($"            {full} v = default;");
            sb.AppendLine($"            ref byte b = ref global::System.Runtime.CompilerServices.Unsafe.As<{full}, byte>(ref v);");
            sb.AppendLine("            return new global::Falcon.DataField[]");
            sb.AppendLine("            {");
            foreach (IFieldSymbol f in type.GetMembers().OfType<IFieldSymbol>())
            {
                if (f.IsStatic || f.IsConst || f.DeclaredAccessibility != Accessibility.Public || f.IsImplicitlyDeclared)
                    continue;
                string kind = DataKindOf(f.Type);
                if (kind.Length == 0)
                    continue;
                string ft = f.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
                sb.AppendLine($"                new(\"{f.Name}\", \"{kind}\", (int)global::System.Runtime.CompilerServices.Unsafe.ByteOffset(ref b, " +
                              $"ref global::System.Runtime.CompilerServices.Unsafe.As<{ft}, byte>(ref global::System.Runtime.CompilerServices.Unsafe.AsRef(in v.{f.Name})))),");
            }
            sb.AppendLine("            };");
            sb.AppendLine("        }");
        }
        sb.AppendLine("        [global::System.Runtime.CompilerServices.ModuleInitializer]");
        sb.AppendLine("        internal static void Register()");
        sb.AppendLine("        {");
        foreach (INamedTypeSymbol type in systems)
            sb.AppendLine($"            global::Falcon.Systems.Register(static () => new global::{type.ToDisplayString()}());");
        // Data components: each public field the editor can show, at its offset in the struct.
        foreach (INamedTypeSymbol type in datas)
        {
            string full = type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
            sb.AppendLine($"            global::Falcon.DataComponents.Register<{full}>(\"{Label(type.Name)}\", DataFields{datas.IndexOf(type)}());");
        }
        foreach (INamedTypeSymbol type in unique)
        {
            string full = type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
            string name = type.ToDisplayString();
            List<Prop> props = PropsOf(spc, type);
            sb.AppendLine(IsModifier(type) ? "            global::Falcon.Level.MeshModifiers.Register(new global::Falcon.ComponentType("
                                           : "            global::Falcon.ComponentTypes.Register(new global::Falcon.ComponentType(");
            sb.AppendLine($"                \"{name}\", \"{Label(type.Name)}\", static () => new {full}(),");
            sb.AppendLine("                new global::Falcon.PropertyInfo[]");
            sb.AppendLine("                {");
            foreach (Prop p in props)
            {
                string enums = p.EnumNames.Length == 0 ? "null" : "new[] { " + string.Join(", ", p.EnumNames.Select(e => "\"" + e + "\"")) + " }";
                sb.AppendLine($"                    new(\"{p.Name}\", \"{p.Kind}\", {p.Min ?? "float.NaN"}, {p.Max ?? "float.NaN"}, {Str(p.Asset)}, {enums}, {Str(p.Tooltip)}, {Str(p.Group)}),");
            }
            sb.AppendLine("                },");
            // Write: the properties as a JSON object's members.
            sb.AppendLine("                static (c, w) =>");
            sb.AppendLine("                {");
            sb.AppendLine($"                    var x = ({full})c;");
            foreach (Prop p in props)
                sb.AppendLine("                    " + WriteOf(p));
            sb.AppendLine("                },");
            // Read: the properties a JSON object has (others keep their values).
            sb.AppendLine("                static (c, o) =>");
            sb.AppendLine("                {");
            sb.AppendLine($"                    var x = ({full})c;");
            if (props.Count > 0)
                sb.AppendLine("                    global::System.Text.Json.JsonElement v;");
            foreach (Prop p in props)
                sb.AppendLine($"                    if (o.TryGetProperty(\"{p.Name}\", out v)) {ReadOf(p)}");
            // (API v34) Its buttons ([Button] methods) and its own Inspector page.
            var buttons = new List<(string name, string label)>();
            for (INamedTypeSymbol? t = type; t != null && t.ToDisplayString() != "Falcon.Component"; t = t.BaseType)
                foreach (IMethodSymbol m in t.GetMembers().OfType<IMethodSymbol>())
                {
                    AttributeData? b = m.GetAttributes().FirstOrDefault(a => a.AttributeClass?.ToDisplayString() == "Falcon.ButtonAttribute");
                    if (b == null || m.IsStatic || m.Parameters.Length != 0 || m.DeclaredAccessibility == Accessibility.Private ||
                        m.DeclaredAccessibility == Accessibility.Protected || buttons.Any(x => x.name == m.Name))
                        continue;
                    string? label = b.ConstructorArguments.Length > 0 ? b.ConstructorArguments[0].Value as string : null;
                    buttons.Add((m.Name, label ?? Label(m.Name)));
                }
            AttributeData? page = type.GetAttributes().FirstOrDefault(a => a.AttributeClass?.ToDisplayString() == "Falcon.InspectorPageAttribute");
            sb.AppendLine("                })");
            sb.AppendLine("            {");
            if (buttons.Count > 0)
            {
                sb.AppendLine("                Buttons = new global::Falcon.ButtonInfo[] { " +
                              string.Join(", ", buttons.Select(b => $"new({Str(b.name)}, {Str(b.label)})")) + " },");
                sb.AppendLine("                Invoke = static (c, m) =>");
                sb.AppendLine("                {");
                sb.AppendLine($"                    var x = ({full})c;");
                sb.AppendLine("                    switch (m)");
                sb.AppendLine("                    {");
                foreach (var b in buttons)
                    sb.AppendLine($"                    case {Str(b.name)}: x.{b.name}(); break;");
                sb.AppendLine("                    }");
                sb.AppendLine("                },");
            }
            if (page != null && page.ConstructorArguments.Length > 0 && page.ConstructorArguments[0].Value is string pagePath)
            {
                int height = page.NamedArguments.Where(a => a.Key == "Height").Select(a => a.Value.Value is int h ? h : 120).DefaultIfEmpty(120).First();
                sb.AppendLine($"                Page = {Str(pagePath)}, PageHeight = {height}, Library = {Str(LibraryOf(type))},");
            }
            // (API v37) Its [Sync] members: the networked simulation's state, in declaration order.
            List<SyncMember> syncs = SyncMembersOf(spc, type);
            if (syncs.Count > 0)
            {
                sb.AppendLine("                SyncWrite = static (c, w) =>");
                sb.AppendLine("                {");
                sb.AppendLine($"                    var x = ({full})c;");
                foreach (SyncMember m in syncs)
                    sb.AppendLine("                    " + SyncWriteOf(m));
                sb.AppendLine("                },");
                sb.AppendLine("                SyncRead = static (global::Falcon.Component c, ref global::Falcon.SyncReader r) =>");
                sb.AppendLine("                {");
                sb.AppendLine($"                    var x = ({full})c;");
                foreach (SyncMember m in syncs)
                    sb.AppendLine("                    " + SyncReadOf(m));
                sb.AppendLine("                },");
            }
            sb.AppendLine("            });");
        }
        // Editor code: menu items and tools.
        foreach (IMethodSymbol m in menus)
        {
            if (!m.IsStatic || m.Parameters.Length != 0 || m.DeclaredAccessibility == Accessibility.Private)
            {
                spc.ReportDiagnostic(Diagnostic.Create(BadMenu, m.Locations.FirstOrDefault(), m.ContainingType.Name, m.Name));
                continue;
            }
            AttributeData attr = m.GetAttributes().First(a => a.AttributeClass?.ToDisplayString() == "Falcon.Editor.EditorMenuAttribute");
            string path = attr.ConstructorArguments.Length > 0 ? attr.ConstructorArguments[0].Value as string ?? m.Name : m.Name;
            string? Named(string key) => attr.NamedArguments.Where(a => a.Key == key).Select(a => a.Value.Value as string).FirstOrDefault();
            string owner = m.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
            sb.AppendLine($"            global::Falcon.Editor.EditorRegistry.Command({Str(path)}, {Str(Named("Shortcut"))}, {Str(Named("Key"))}, " +
                          $"{Str(Named("Modes"))}, {Str(Named("Icon"))}, {Str(Named("Wheel"))}, {Str(Named("Help"))}, static () => {owner}.{m.Name}());");
        }
        foreach (INamedTypeSymbol t in tools)
            sb.AppendLine($"            global::Falcon.Editor.EditorRegistry.Tool(static () => new {t.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)}());");
        foreach (INamedTypeSymbol t in windows)
        {
            AttributeData? a = t.GetAttributes().FirstOrDefault(x => x.AttributeClass?.ToDisplayString() == "Falcon.Editor.EditorWindowAttribute");
            string menu = a != null && a.ConstructorArguments.Length > 0 ? a.ConstructorArguments[0].Value as string ?? "Tools/" + t.Name : "Tools/" + t.Name;
            string Named(string key, string fallback) =>
                a?.NamedArguments.Where(n => n.Key == key).Select(n => n.Value.Value?.ToString()).FirstOrDefault() ?? fallback;
            string title = Named("Title", Label(t.Name)), page = Named("Page", "index.html");
            string width = Named("Width", "360"), height = Named("Height", "420");
            sb.AppendLine($"            global::Falcon.Editor.EditorRegistry.Window({Str(menu)}, {Str(title)}, {Str(LibraryOf(t))}, {Str(page)}, {width}, {height}, " +
                          $"static () => new {t.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)}());");
        }
        sb.AppendLine("        }");
        sb.AppendLine("    }");
        sb.AppendLine("}");
        spc.AddSource("ScriptComponents.g.cs", sb.ToString());
    }

    private static string Str(string? s) => s == null ? "null" : "@\"" + s.Replace("\"", "\"\"") + "\"";

    // "PlayerController" -> "Player Controller"
    private static string Label(string name)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < name.Length; ++i)
        {
            if (i > 0 && char.IsUpper(name[i]) && !char.IsUpper(name[i - 1]))
                sb.Append(' ');
            sb.Append(name[i]);
        }
        return sb.ToString();
    }

    private static List<Prop> PropsOf(SourceProductionContext spc, INamedTypeSymbol type)
    {
        var props = new List<Prop>();
        var chain = new List<INamedTypeSymbol>();
        for (INamedTypeSymbol? t = type; t != null && t.ToDisplayString() != "Falcon.Component"; t = t.BaseType)
            chain.Insert(0, t);  // (base classes' properties first)
        foreach (INamedTypeSymbol t in chain)
            foreach (ISymbol m in t.GetMembers())
            {
                if (m.DeclaredAccessibility != Accessibility.Public || m.IsStatic)
                    continue;
                ITypeSymbol? mt = m switch
                {
                    IFieldSymbol f when !f.IsReadOnly && !f.IsConst => f.Type,
                    IPropertySymbol p when p.SetMethod != null && p.GetMethod != null && !p.IsIndexer => p.Type,
                    _ => null,
                };
                if (mt == null)
                    continue;
                AttributeData? attr = m.GetAttributes().FirstOrDefault(a => a.AttributeClass?.ToDisplayString() == "Falcon.PropertyAttribute");
                if (attr == null)
                    continue;
                var prop = new Prop { Name = m.Name, TypeName = mt.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) };
                prop.Kind = KindOf(mt);
                if (prop.Kind == "")
                {
                    spc.ReportDiagnostic(Diagnostic.Create(Unsupported, m.Locations.FirstOrDefault(), type.Name, m.Name, mt.ToDisplayString()));
                    continue;
                }
                if (mt.TypeKind == TypeKind.Enum)
                    prop.EnumNames = mt.GetMembers().OfType<IFieldSymbol>().Where(f => f.IsConst).Select(f => f.Name).ToArray();
                foreach (KeyValuePair<string, TypedConstant> named in attr.NamedArguments)
                {
                    if (named.Key == "Tooltip")
                        prop.Tooltip = named.Value.Value as string;
                    else if (named.Key == "Group")
                        prop.Group = named.Value.Value as string;
                }
                foreach (AttributeData a in m.GetAttributes())
                {
                    string? an = a.AttributeClass?.ToDisplayString();
                    if (an == "Falcon.RangeAttribute" && a.ConstructorArguments.Length == 2)
                    {
                        prop.Min = Number(a.ConstructorArguments[0]);
                        prop.Max = Number(a.ConstructorArguments[1]);
                    }
                    else if (an == "Falcon.AssetAttribute" && a.ConstructorArguments.Length == 1)
                    {
                        prop.Asset = a.ConstructorArguments[0].Value as string;
                    }
                }
                props.Add(prop);
            }
        return props;
    }

    private sealed class SyncMember
    {
        public string Name = "", Kind = "", TypeName = "";
    }

    // [Sync] members (base classes' first): their kind, or a FALCON003 warning.
    private static List<SyncMember> SyncMembersOf(SourceProductionContext spc, INamedTypeSymbol type)
    {
        var members = new List<SyncMember>();
        var chain = new List<INamedTypeSymbol>();
        for (INamedTypeSymbol? t = type; t != null && t.ToDisplayString() != "Falcon.Component"; t = t.BaseType)
            chain.Insert(0, t);
        foreach (INamedTypeSymbol t in chain)
            foreach (ISymbol m in t.GetMembers())
            {
                if (m.IsStatic || !m.GetAttributes().Any(a => a.AttributeClass?.ToDisplayString() == "Falcon.SyncAttribute"))
                    continue;
                ITypeSymbol? mt = m switch
                {
                    IFieldSymbol f when !f.IsReadOnly && !f.IsConst => f.Type,
                    IPropertySymbol p when p.SetMethod != null && p.GetMethod != null && !p.IsIndexer &&
                                           p.SetMethod.DeclaredAccessibility != Accessibility.Private &&
                                           p.GetMethod.DeclaredAccessibility != Accessibility.Private => p.Type,
                    _ => null,
                };
                if (mt == null)
                {
                    spc.ReportDiagnostic(Diagnostic.Create(BadSync, m.Locations.FirstOrDefault(), type.Name, m.Name,
                                                           "it must be a field (not readonly) or a property with a get and a set"));
                    continue;
                }
                if (m.DeclaredAccessibility == Accessibility.Private || m.DeclaredAccessibility == Accessibility.Protected)
                {
                    spc.ReportDiagnostic(Diagnostic.Create(BadSync, m.Locations.FirstOrDefault(), type.Name, m.Name,
                                                           "it must not be private or protected (the generated code reads and writes it)"));
                    continue;
                }
                string kind = mt.TypeKind == TypeKind.Enum ? "enum" : KindOf(mt);
                if (kind == "")
                    kind = mt.SpecialType switch
                    {
                        SpecialType.System_Byte => "byte",
                        SpecialType.System_UInt64 => "ulong",
                        _ => "",
                    };
                if (kind == "")
                {
                    spc.ReportDiagnostic(Diagnostic.Create(BadSync, m.Locations.FirstOrDefault(), type.Name, m.Name,
                                                           $"a {mt.ToDisplayString()} can't be synced (bool, byte, int, uint, long, ulong, float, double, an enum, string, Vector2/3/4, Quaternion or Entity can)"));
                    continue;
                }
                members.Add(new SyncMember { Name = m.Name, Kind = kind, TypeName = mt.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) });
            }
        return members;
    }

    private static string SyncWriteOf(SyncMember m) => m.Kind switch
    {
        "enum" => $"w.Write((long)x.{m.Name});",
        _ => $"w.Write(x.{m.Name});",
    };

    private static string SyncReadOf(SyncMember m) => m.Kind switch
    {
        "bool" => $"x.{m.Name} = r.Bool();",
        "byte" => $"x.{m.Name} = r.Byte();",
        "int" => $"x.{m.Name} = r.Int();",
        "uint" => $"x.{m.Name} = r.UInt();",
        "long" => $"x.{m.Name} = r.Long();",
        "ulong" => $"x.{m.Name} = r.ULong();",
        "float" => $"x.{m.Name} = r.Float();",
        "double" => $"x.{m.Name} = r.Double();",
        "string" => $"x.{m.Name} = r.String();",
        "enum" => $"x.{m.Name} = ({m.TypeName})r.Long();",
        "vec2" => $"x.{m.Name} = r.Vector2();",
        "vec3" => $"x.{m.Name} = r.Vector3();",
        "vec4" => $"x.{m.Name} = r.Vector4();",
        "quat" => $"x.{m.Name} = r.Quaternion();",
        "entity" => $"x.{m.Name} = r.Entity();",
        _ => "{ }",
    };

    private static string Number(TypedConstant c) =>
        System.Convert.ToDouble(c.Value, System.Globalization.CultureInfo.InvariantCulture).ToString("R", System.Globalization.CultureInfo.InvariantCulture) + "f";

    private static string KindOf(ITypeSymbol t)
    {
        if (t.TypeKind == TypeKind.Enum)
            return "enum";
        switch (t.SpecialType)
        {
        case SpecialType.System_Single: return "float";
        case SpecialType.System_Double: return "double";
        case SpecialType.System_Int32: return "int";
        case SpecialType.System_UInt32: return "uint";
        case SpecialType.System_Int64: return "long";
        case SpecialType.System_Boolean: return "bool";
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

    private static string WriteOf(Prop p) => p.Kind switch
    {
        "float" or "double" or "int" or "uint" or "long" => $"w.WriteNumber(\"{p.Name}\", x.{p.Name});",
        "bool" => $"w.WriteBoolean(\"{p.Name}\", x.{p.Name});",
        "string" => $"w.WriteString(\"{p.Name}\", x.{p.Name} ?? \"\");",
        "enum" => $"w.WriteString(\"{p.Name}\", x.{p.Name}.ToString());",
        _ => $"global::Falcon.ComponentJson.Write(w, \"{p.Name}\", x.{p.Name});",
    };

    private static string ReadOf(Prop p) => p.Kind switch
    {
        "float" => $"x.{p.Name} = (float)global::Falcon.ComponentJson.Number(v, x.{p.Name});",
        "double" => $"x.{p.Name} = global::Falcon.ComponentJson.Number(v, x.{p.Name});",
        "int" => $"x.{p.Name} = (int)global::Falcon.ComponentJson.Number(v, x.{p.Name});",
        "uint" => $"x.{p.Name} = (uint)global::Falcon.ComponentJson.Number(v, x.{p.Name});",
        "long" => $"x.{p.Name} = (long)global::Falcon.ComponentJson.Number(v, x.{p.Name});",
        "bool" => $"x.{p.Name} = v.ValueKind == global::System.Text.Json.JsonValueKind.True || (v.ValueKind != global::System.Text.Json.JsonValueKind.False && x.{p.Name});",
        "string" => $"x.{p.Name} = v.ValueKind == global::System.Text.Json.JsonValueKind.String ? v.GetString() ?? \"\" : x.{p.Name};",
        "enum" => $"{{ if (v.ValueKind == global::System.Text.Json.JsonValueKind.String && global::System.Enum.TryParse<{p.TypeName}>(v.GetString(), out var e)) x.{p.Name} = e; }}",
        "vec2" => $"x.{p.Name} = global::Falcon.ComponentJson.Vector2(v, x.{p.Name});",
        "vec3" => $"x.{p.Name} = global::Falcon.ComponentJson.Vector3(v, x.{p.Name});",
        "vec4" => $"x.{p.Name} = global::Falcon.ComponentJson.Vector4(v, x.{p.Name});",
        "quat" => $"x.{p.Name} = global::Falcon.ComponentJson.Quaternion(v, x.{p.Name});",
        "entity" => $"x.{p.Name} = global::Falcon.ComponentJson.Entity(v, x.{p.Name});",
        _ => "{ }",
    };
}
