using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using System.Text.Json;
using Falcon.Editor;

namespace Falcon.Level;

// The level tools' own commands (ProBuilder's operations, Blender's keys): on the level toolbar, the
// Alt+Q and Shift+A wheels and the command palette, rebindable in the keymap. They act on the
// selected level mesh's chosen elements (or, in the object mode, on the whole mesh) through the
// engine's mesh operations (LevelOps), each an undo step. Registered by the editor host
// (EditorRegistry.EnsureBuiltins) after the code's own commands, so a project's keys come first.
internal static class LevelCommands
{
    internal static void Register()
    {
        // Modal: the mouse (or a typed number) sets the amount, click / Enter confirms.
        Command("Mesh/Extrude", "E", "edge|face", "extrude", "ops", "Pull the chosen faces out along their normals (edges: a strip off open edges)",
                () => Modal("Extrude", t => t.Mode == ElementMode.Edge ? "extrudeEdges" : "extrude", "distance", float.NegativeInfinity));
        Command("Mesh/Inset", "I", "face", "inset", "ops", "Shrink the chosen faces inward, a ring of faces around each",
                () => Modal("Inset", _ => "inset", "amount", 0.001f));
        Command("Mesh/Bevel", "B", "edge", "bevel", "ops", "Cut the chosen edges into chamfer faces: the mouse sets the width, the wheel the segments (round), C round / flat / curved in",
                () => Modal("Bevel", _ => "bevel", "amount", 0.001f, t => new BevelModal(t))).Options = new() { s_bevelSegments, s_bevelCurve };
        // At once.
        Command("Mesh/Loop Cut", "Alt+R", "edge", "loopCut", "ops", "A new edge loop across the ring of the chosen edge", () => Run("Loop Cut", "loopCut"));
        Command("Mesh/Connect", "J", "vertex|edge", "connect", "ops", "Split faces between the chosen vertices, or the middles of the chosen edges",
                () => Run("Connect", "connect"));
        Command("Mesh/Subdivide", null, "face", "subdivide", "ops", "Each chosen face into quads", () => Run("Subdivide", "subdivide"));
        Command("Mesh/Merge", "M", "vertex", "merge", "ops", "The chosen vertices into one at their centre", () => Run("Merge", "merge"));
        CommandOption weldDistance = CommandOption.Number("Distance", 0.01, 0.0001, 1, "m");
        Command("Mesh/Weld", null, "object|vertex", "weld", null, "Vertices closer than the distance (a centimetre) into one (the chosen, or all)",
                () => Run("Weld", "weld", w => w.WriteNumber("distance", weldDistance.Value), "Mesh/Weld")).Options = new() { weldDistance };
        Command("Mesh/Fill", "Alt+F", "vertex|edge", "fill", "ops", "Two edges bridged, an open edge's hole filled, or a face over the chosen vertices",
                () => Run("Fill", "fill"));
        Command("Mesh/Delete", "X", "vertex|edge|face", "trash", "ops", "Delete the chosen elements (and the faces using them)", () => Run("Delete", "delete"));
        Command("Mesh/Detach", "P", "face", "detach", "ops", "Take the chosen faces out as a level mesh of their own", Detach);
        Command("Mesh/Flip Normals", "Alt+N", "object|face", "flip", "ops", "Turn the chosen faces (in the object mode: the whole mesh, a room) inside out",
                () => Run("Flip Normals", "flip"));
        Command("Mesh/Conform Normals", null, "object|face", "conform", null, "Wind every face as its neighbours are", () => Run("Conform Normals", "conform"));
        Command("Mesh/Triangulate", null, "object|face", "triangulate", null, "Faces into triangles", () => Run("Triangulate", "triangulate"));
        Command("Mesh/Quadrangulate", null, "object|face", "quad", null, "Pairs of triangles into quads", () => Run("Quadrangulate", "quadrangulate"));
        Command("Mesh/Merge Faces", null, "face", "mergeFaces", null, "Adjacent chosen faces into one", () => Run("Merge Faces", "mergeFaces"));
        CommandOption smoothAngle = CommandOption.Number("Angle", 40, 1, 180, "°");
        Command("Mesh/Auto Smooth", null, "object", "autoSmooth", null, "Smooth faces meeting at under the angle (40 degrees), the rest flat",
                () => Run("Auto Smooth", "autoSmooth", w => w.WriteNumber("angle", smoothAngle.Value), "Mesh/Auto Smooth")).Options = new() { smoothAngle };
        Command("Mesh/Smooth Faces", null, "face", "smooth", null, "The chosen faces shaded smooth together", () => Run("Smooth", "smooth", w => w.WriteNumber("group", 1)));
        Command("Mesh/Flat Faces", null, "face", "flat", null, "The chosen faces shaded flat", () => Run("Flat", "smooth", w => w.WriteNumber("group", 0)));
        // Selection.
        Command("Select/Loop", "L", "edge", "loopSelect", "ops", "The edge loop through the chosen edges", () => Select("loop"));
        Command("Select/Ring", "Shift+L", "edge", "ringSelect", "ops", "The edge ring through the chosen edges", () => Select("ring"));
        Command("Select/Grow", "=", "vertex|edge|face", "grow", null, "Add the neighbours of what is chosen", () => Select("grow"));
        Command("Select/Shrink", "-", "vertex|edge|face", "shrink", null, "Drop the border of what is chosen", () => Select("shrink"));
        Command("Select/By Material", null, "face", "byMaterial", null, "Every face with a chosen face's material", () => Select("byMaterial"));
        CommandOption normalAngle = CommandOption.Number("Angle", 10, 0.5, 90, "°");
        Command("Select/By Normal", null, "face", "byNormal", null, "Every face facing as a chosen face does (within the angle: 10 degrees)",
                () => Select("byNormal", w => w.WriteNumber("angle", normalAngle.Value))).Options = new() { normalAngle };
        // Shapes: click a corner, click the opposite one, then the height.
        foreach ((string name, string label, string icon) in s_shapes)
        {
            List<CommandOption> options = ShapeOptions(name);
            Command($"Create/{label}", null, "object", icon, "create", $"Draw a {label.ToLowerInvariant()}: two corners on the ground, then its height",
                    () => ModalOperation.Begin(new ShapeTool(name, label, options))).Options = options.Count > 0 ? options : null;
        }
        // UVs (the Shift+U wheel), through the editor (editor.level.op: the chosen faces, the
        // entity's transform for World faces, one undo step).
        Command("UV/Editor", "Alt+U", "object|face", "uvEditor", "uv", "Show the chosen faces' UVs: drag, turn and scale them", () => Page("editor.level.uvEditor"));
        Command("UV/Unwrap", "U", "object|face", "uvUnwrap", "uv", "Unfold the faces into flat charts (their own UVs, a metre a tile)", () => Bridge("uvUnwrap"));
        Command("UV/Fit", null, "face", "uvFit", "uv", "The texture once across each chosen face", () => Bridge("uvFit"));
        Command("UV/Align to Edge", null, "face", "uvAlign", "uv", "Turn each face's texture to run along its longest edge", () => Bridge("uvAlign"));
        Command("UV/Match Neighbours", null, "face", "uvMatch", "uv", "Carry the first chosen face's texture on across the others' shared edges",
                () => Bridge("uvMatch"));
        Command("UV/Texture Lock", null, "object|face", "uvWorld", "uv", "Project in world space: the texture stays put as the mesh moves, neighbours line up",
                () => Bridge("uv", w => w.WriteString("mode", "world")));
        Command("UV/Local Projection", null, "object|face", "uvLocal", "uv", "Project in the mesh's own space: the texture moves with it",
                () => Bridge("uv", w => w.WriteString("mode", "local")));
        Command("UV/Rotate 90", null, "face", "uvRotate", "uv", "Turn the texture a quarter", () => Bridge("uvNudge", w => w.WriteNumber("rotation", 90)));
        Command("UV/Flip U", null, "face", "uvFlip", "uv", "Mirror the texture across", () => Bridge("uvNudge", w => Pair(w, "scale", -1, 1)));
        Command("UV/Flip V", null, "face", "uvFlipV", "uv", "Mirror the texture up and down", () => Bridge("uvNudge", w => Pair(w, "scale", 1, -1)));
        Command("UV/Reset", null, "object|face", "uvReset", "uv", "Back to a local projection, a metre a tile", () => Bridge("uvReset"));
        // Trim sheets (.ftrim): strips of one texture for edges, bevels and bands.
        Command("UV/Trim Sheets", "Alt+T", "object|face", "trim", "uv", "Show the trim sheets: click a strip to put the chosen faces on it",
                () => Page("editor.level.trimPanel"));
        Command("UV/Trim Again", "T", "face", "trimAgain", "ops", "The chosen faces onto the last trim strip used", () => Page("editor.level.trim"));
        Command("UV/Make Starter Trims", null, null, "trim", null, "A trim sheet to start from in Assets/Trims (metal edges, bolts, seams, concrete, planks, stripes, grime)",
                () => Page("editor.trim.starter"));
        // Walls and rooms (parametric): click corners on the ground, Enter to finish; openings: a click on a wall.
        List<CommandOption> wallOptions = WallOptions(), roomOptions = WallOptions();
        Command("Create/Wall", null, "object", "wall", "create", "Draw a wall: click its corners on the ground, Enter to finish",
                () => ModalOperation.Begin(new WallTool(false, wallOptions))).Options = wallOptions;
        Command("Create/Room", null, "object", "room", "create", "Draw a room: click its corners, Enter to close it (walls, floor, ceiling)",
                () => ModalOperation.Begin(new WallTool(true, roomOptions))).Options = roomOptions;
        Command("Create/Roof", null, "object", "roof", "create", "A roof over the selected room (its style, pitch and overhang in the Inspector)",
                () => EditorApp.Send("editor.level.roof.create", _ => { }));
        List<CommandOption> door = OpeningOptions(1.0, 2.1, 0.0), window = OpeningOptions(1.2, 1.2, 0.9);
        Command("Create/Door", null, "object", "door", "create", "Click a wall: a door through it (1 x 2.1 m, from the floor)",
                () => ModalOperation.Begin(new OpeningTool("Door", door))).Options = door;
        Command("Create/Window", null, "object", "window", "create", "Click a wall: a window through it (1.2 x 1.2 m, 0.9 m up)",
                () => ModalOperation.Begin(new OpeningTool("Window", window))).Options = window;
        Command("Wall/Convert to Mesh", null, "object", "convert", "cut", "The selected wall becomes a plain level mesh (its openings plain cuts)",
                () => Page("editor.level.convert"));
        // Cut volumes (live cuts): made at the view's centre, sized by their transform; what they do.
        foreach ((string shape, string label, string icon) in new[] { ("box", "Box", "cutBox"), ("cylinder", "Cylinder", "cutCylinder"), ("sphere", "Sphere", "cutSphere") })
            Command($"Create/Cut {label}", null, "object", icon, "create", $"A {label.ToLowerInvariant()} that cuts the level meshes it overlaps (live, sized by its scale)",
                    () => EditorApp.Send("editor.level.cut.create", w => w.WriteString("shape", shape)));
        Command("Create/Cut Damage", null, "object", "cutDamage", "create",
                "A lumpy ball that breaks the level meshes it overlaps (live, sized by its scale)",
                () => EditorApp.Send("editor.level.cut.create", w => { w.WriteString("shape", "damage"); w.WriteNumber("seed", 1); }));
        Command("Cut/Subtract", null, "object", "subtract", "cut", "The selected cut takes away what it overlaps",
                () => EditorApp.Send("editor.level.cut", w => w.WriteBoolean("intersect", false)));
        Command("Cut/Intersect", null, "object", "intersect", "cut", "The selected cut keeps only what is inside it",
                () => EditorApp.Send("editor.level.cut", w => w.WriteBoolean("intersect", true)));
        Command("Cut/Cap With Picked Material", null, "object", "capPick", "cut", "The faces the selected cut makes get the eyedropper's material (Alt+C)",
                () => EditorApp.Send("editor.level.cut", w => w.WriteBoolean("capPicked", true)));
        // Painting (the scene view's level paint brush; [ ] its size, the paint panel the rest):
        // the surface materials (the mask), erasing them, the vertex colours.
        Command("Paint/Layer Brush", "Shift+B", "object|face", "paint", "paint", "Paint the selected mesh's surface materials on (the panel lists them, + adds one, and says what each is for)",
                () => PaintTool(0));
        Command("Paint/Erase Layers", null, "object|face", "eraser", "paint", "Paint the surface materials off again", () => PaintTool(3));
        Command("Paint/Vertex Colours", null, "object|face", "tint", "paint", "Paint the vertices' colour (tint, and occlusion in its alpha)",
                () => PaintTool(4));
        Command("Paint/Decal Brush", null, null, "decal", "paint", "Stamp decals along a stroke on any surface (turned and sized at random; the panel picks the material)",
                () => PaintTool(5));
        Command("Paint/Scatter Brush", null, null, "scatter", "paint",
                "Scatter the prefab shelf's pieces over any surface along a stroke (Shift: erase them); the panel sets how many and how big",
                () => PaintTool(7));
        Command("Paint/Damage Brush", null, null, "crack", "paint", "Cut lumpy holes into level meshes along a stroke (the panel: chipped edges, broken corners)",
                () => PaintTool(6));
        // (its settings: the chips' spacing in metres, 0 from the brush's size; how far they stray and
        // vary in size; the share of their places that get one; the paint panel's Damage shows them too)
        CommandOption damageSpacing = CommandOption.Number("Spacing", 0, 0, 50, "m"), damageRandomness = CommandOption.Number("Randomness", 0, 0, 100, "%"),
                      damageDensity = CommandOption.Number("Density", 100, 0, 100, "%");
        Command("Paint/Damage Selection", null, "object|vertex|edge|face", "damageSelection", "paint",
                "Damage the chosen corners (vertices), along the chosen edges or over the chosen faces; a whole mesh along its sharp edges; a spline along its curve. Its settings: Spacing (0: from the brush's size), Randomness, Density. The damage brush's size, strength, rubble and inside apply",
                () => EditorApp.Send("editor.level.damage", w =>
                {
                    w.WriteNumber("spacing", damageSpacing.Value);
                    w.WriteNumber("randomness", damageRandomness.Value);
                    w.WriteNumber("density", damageDensity.Value);
                })).Options = new() { damageSpacing, damageRandomness, damageDensity };
        Command("Paint/Fill Layer 1", null, "object|face", "fillLayer", null, "The first surface material everywhere on the selected mesh",
                () => EditorApp.Send("editor.level.mask", w => { w.WriteString("op", "fill"); w.WriteNumber("channel", 0); w.WriteNumber("value", 1); }));
        // Arranging the selection (Shift+G): aligned to the last selected, spread evenly, dropped down.
        foreach ((string axis, int a) in new[] { ("X", 0), ("Y", 1), ("Z", 2) })
        {
            Command($"Arrange/Align Min {axis}", null, null, "alignMin", "arrange", $"The selection's {axis} minimum at the last selected one's", () => Arrange("align", a, -1));
            Command($"Arrange/Align Centre {axis}", null, null, "alignCentre", "arrange", $"The selection's {axis} centre at the last selected one's", () => Arrange("align", a, 0));
            Command($"Arrange/Align Max {axis}", null, null, "alignMax", "arrange", $"The selection's {axis} maximum at the last selected one's", () => Arrange("align", a, 1));
        }
        Command("Arrange/Distribute X", null, null, "distribute", "arrange", "The selection spread evenly along x between its first and last", () => Arrange("distribute", 0, 0));
        Command("Arrange/Distribute Z", null, null, "distribute", "arrange", "The selection spread evenly along z between its first and last", () => Arrange("distribute", 2, 0));
        Command("Arrange/Drop to Ground", "End", null, "ground", "arrange", "Each selected one down onto whatever is under it", () => Arrange("ground", 1, 0));
        Command("Arrange/Snap Together", null, null, "magnet", "arrange", "Each selected kit piece onto the snap point nearest one of its own (within 40 cm)",
                () => Arrange("snap", 0, 0));
        Command("Create/Snap Point", null, "object", "snapPoint", "create",
                "A snap point on the selected kit piece (move and turn it: it faces out along +z); pieces moved near another's click onto it",
                () => Page("editor.level.snapPoint"));
        Command("Arrange/Group", null, null, "group", "arrange", "The selection under a new empty (Ctrl+G)", () => Page("editor.group"));
        Command("Arrange/Ungroup", null, null, "ungroup", "arrange", "The selected group's children out of it, the empty gone (Ctrl+Shift+G)", () => Page("editor.ungroup"));
        // The sample level (to take apart).
        Command("Help/Make Sample Level", null, null, "sample", null,
                "A small level built with the level tools in this scene: a room with a door and a window, worn concrete, a road with fence posts, volumes, navigation",
                SampleLevel.Make);
        Command("Help/Install Prototype Materials", null, null, "prototype", null,
                "Kenney's prototype textures (grids, checkers, stairs, door and window guides in six colours) into Assets/Prototype again, with their materials; level meshes' faces without a material use Light Grid",
                () => EditorApp.Send("editor.level.prototype", _ => { }));
        Command("Help/Make Starter Kit", null, null, "kit", null,
                "Door, double door, window and vent prefabs in Assets/Kit, each an opening with its frame: drop one on a wall",
                StarterKit.Make);
        // The view: orthographic, its sides; the ruler.
        Command("View/Orthographic", null, null, "ortho", "view", "Orthographic or perspective (Numpad 5): parallel lines stay parallel",
                () => EditorApp.Send("editor.view", _ => { }));
        foreach ((string label, string look, string key) in new[] { ("Front", "front", "Numpad 1"), ("Back", "back", "Ctrl+Numpad 1"), ("Right", "right", "Numpad 3"),
                                                                     ("Left", "left", "Ctrl+Numpad 3"), ("Top", "top", "Numpad 7"), ("Bottom", "bottom", "Ctrl+Numpad 7") })
            Command($"View/{label}", null, null, "viewSide", "view", $"Look at the pivot from the {look} ({key})",
                    () => EditorApp.Send("editor.view", w => w.WriteString("look", look)));
        Command("View/Measure", null, null, "ruler", "view", "A ruler: drag between two points on surfaces (V: onto vertices); its length in the hints bar",
                () => EditorApp.Send("editor.tool.set", w => w.WriteString("tool", "measure")));
        // Gameplay volumes: level meshes that mark space (not drawn in the game).
        foreach ((string label, string kind, string icon, string help) in new[]
                 {
                     ("Trigger", "trigger", "trigger", "A trigger: scripts hear what enters and leaves it"),
                     ("Kill", "kill", "kill", "A kill volume: what falls in dies (the game's scripts decide how)"),
                     ("Ladder", "ladder", "ladder", "A ladder: characters in it climb"),
                     ("Audio", "audio", "audioZone", "An audio zone (its tag names the ambience or reverb)"),
                     ("Nav Blocker", "navBlocker", "navBlocker", "Keeps the navigation mesh out"),
                     ("Nav Area", "navArea", "navArea", "Costs more to walk through: paths go round it"),
                 })
        {
            Command($"Create/Volume/{label}", null, "object", icon, "create", help + " (a 2 m box: shape it with the level tools)",
                    () => EditorApp.Send("editor.level.volume", w => { w.WriteBoolean("create", true); w.WriteString("kind", kind); }));
            Command($"Volume/Make {label}", null, "object", icon, null, $"The selected level mesh a {label.ToLowerInvariant()} volume",
                    () => EditorApp.Send("editor.level.volume", w => w.WriteString("kind", kind)));
        }
        // Navigation: the scene's navigation mesh (Project Settings: the agent).
        Command("Navigation/Bake", null, null, "navBake", null, "Bake the scene's navigation mesh (saved beside it; games' Navigation queries use it)",
                () => Page("editor.nav.bake"));
        Command("Navigation/Show Navigation Mesh", null, null, "eye", null, "The navigation mesh's polygons drawn over the view (again: hidden)",
                () => Page("editor.nav.show"));
        Command("Navigation/Clear", null, null, "trash", null, "The scene's navigation mesh taken away (its file deleted)", () => Page("editor.nav.clear"));
        // Splines: the Spline tool (click points on any surface; the Inspector's Spline lofts it).
        Command("Create/Spline", null, "object", "spline", "create", "Draw a spline: click its points on any surface, drag them, Enter to finish",
                () => SplineTool(""));
        Command("Create/Road", null, "object", "road", "create", "Draw a road (a 4 m slab along a spline)", () => SplineTool("road"));
        Command("Create/Kerb", null, "object", "kerb", "create", "Draw a kerb along a spline", () => SplineTool("kerb"));
        Command("Create/Spline Wall", null, "object", "splineWall", "create", "Draw a thin wall along a curve", () => SplineTool("wall"));
        Command("Create/Rail", null, "object", "rail", "create", "Draw a rail along a spline", () => SplineTool("rail"));
        Command("Create/Spline Pipe", null, "object", "splinePipe", "create", "Draw a pipe along a spline", () => SplineTool("pipe"));
        Command("Spline/Edit Points", null, "object", "splinePoints", null, "The selected spline's points: drag them, Ctrl+click to add one between, X over one to take it out, V a corner",
                () => EditorApp.Send("editor.tool.set", w => w.WriteString("tool", "spline")));
        // Modifiers (Shift+M; the Inspector's Modifiers edits them): on the selected level mesh's stack.
        Command("Modifier/Mirror", null, "object", "mirror", "modifiers", "Mirror the selected level mesh across x (its other half follows your edits)",
                () => AddModifier("mirror"));
        Command("Modifier/Array", null, "object", "array", "modifiers", "Copies of it in a row, each a size along", () => AddModifier("array", "linear"));
        Command("Modifier/Radial Array", null, "object", "radialArray", "modifiers", "Copies of it around y, two metres out", () => AddModifier("array", "radial"));
        Command("Modifier/Grid Array", null, "object", "gridArray", "modifiers", "Copies of it in a 3 x 3 grid across x and z", () => AddModifier("array", "grid"));
        Command("Modifier/Solidify", null, "object", "solidify", "modifiers", "Thickness for a surface (10 cm; a closed mesh: a hollow shell)",
                () => AddModifier("solidify"));
        Command("Modifier/Bevel", null, "object", "bevelModifier", "modifiers", "Its sharp edges chamfered (5 cm), its corners filled", () => AddModifier("bevel"));
        Command("Modifier/Bend", null, "object", "bend", "modifiers", "Bent along y", () => AddModifier("deform", "bend"));
        Command("Modifier/Twist", null, "object", "twist", "modifiers", "Twisted about y", () => AddModifier("deform", "twist"));
        Command("Modifier/Taper", null, "object", "taper", "modifiers", "Tapered to half at its top", () => AddModifier("deform", "taper"));
        Command("Modifier/Follow Spline", null, "object", "followSpline", "modifiers",
                "Bend the selected level mesh along the spline selected with it (end to end, filling it)", () => AddModifier("curve", "bend"));
        Command("Modifier/Copies Along Spline", null, "object", "copiesSpline", "modifiers",
                "Copies of the selected level mesh along the spline selected with it (fence posts, lamps)", () => AddModifier("curve", "copies"));
        Command("Modifier/Apply All", null, "object", "applyAll", "modifiers", "Its whole modifier stack into the mesh itself",
                () => EditorApp.Send("editor.level.modifier", w => { w.WriteString("op", "apply"); w.WriteBoolean("all", true); }));
        // Live wear: made again as the mesh is cut and edited and as the level is built around it.
        Command("Paint/Edge Wear", null, "object|face", "wear", "paint", "Wear along the selected mesh's sharp outside edges in its first surface material, kept up as you cut and edit it",
                () => LiveWear("edges"));
        Command("Paint/Grime", null, "object|face", "grime", "paint",
                "A stain in the selected mesh's inside corners, at its foot and where other level meshes meet it, in its second surface material, kept up as you build",
                () => LiveWear("grime"));
        Command("Paint/Streaks", null, "object|face", "streaks", "paint", "Streaks running down the selected mesh's sides below its ledges and sills, in its second surface material",
                () => LiveWear("streaks"));
        Command("Paint/Bake Wear", null, "object|face", "bake", null, "The selected mesh's live wear into its painted mask (it stops following the mesh)",
                () => EditorApp.Send("editor.level.wear", w => w.WriteString("op", "bake")));
        Command("Paint/Make Starter Smart Materials", null, null, "material", null,
                "Smart material presets in Assets/Smart (old concrete, chipped paint, rusty steel): the paint panel applies them",
                () => Page("editor.level.smart.starter"));
        Command("Paint/Clear Mask", null, "object|face", "clearMask", null, "No layer anywhere (the mesh's own materials)",
                () => EditorApp.Send("editor.level.mask", w => w.WriteString("op", "clear")));
        // Materials: the eyedropper (the face under the mouse), applied to the chosen faces.
        Command("Material/Pick Under Mouse", "Alt+C", null, "eyedropper", null, "Take the material of the level mesh face under the mouse",
                () => Page("editor.level.pickMaterial"));
        Command("Material/Apply Picked", "Alt+V", "object|face", "applyMaterial", "ops", "Put the picked material on the chosen faces (the object mode: all)",
                () => EditorApp.Send("editor.level.material", _ => { }));
        foreach (EditorRegistry.MenuItem m in EditorRegistry.Menus)
            if (m.Builtin)
                Place(m);
    }

    // ---- the Level window's sections: the first entry whose path starts the command's says where it
    // goes (group, heading), what it needs selected, and its label there (null: the path's last part).
    private static readonly (string Prefix, string Group, string Section, string? Needs, string? Label)[] s_places =
    {
        ("Mesh/Auto Smooth", "edit", "Shading", "elements", null), ("Mesh/Smooth Faces", "edit", "Shading", "elements", "Smooth"),
        ("Mesh/Flat Faces", "edit", "Shading", "elements", "Flat"),
        ("Mesh/Flip Normals", "edit", "Normals", "elements", null), ("Mesh/Conform Normals", "edit", "Normals", "elements", null),
        ("Mesh/Triangulate", "edit", "Topology", "elements", null), ("Mesh/Quadrangulate", "edit", "Topology", "elements", null),
        ("Mesh/Merge Faces", "edit", "Topology", "elements", null), ("Mesh/Weld", "edit", "Topology", "elements", null),
        ("Mesh/", "edit", "Geometry", "elements", null),
        ("Select/", "select", "Select", "elements", null),
        ("Create/Volume/", "create", "Volumes", null, null),
        ("Create/Cut Damage", "create", "Cuts", null, "Damage"), ("Create/Cut ", "create", "Cuts", null, null),
        ("Create/Spline Pipe", "create", "Splines", null, "Pipe"), ("Create/Spline Wall", "create", "Splines", null, "Wall"),
        ("Create/Spline", "create", "Splines", null, null), ("Create/Road", "create", "Splines", null, null),
        ("Create/Kerb", "create", "Splines", null, null), ("Create/Rail", "create", "Splines", null, null),
        ("Create/Wall", "create", "Architecture", null, null), ("Create/Room", "create", "Architecture", null, null),
        ("Create/Roof", "create", "Architecture", "wall", null), ("Create/Door", "create", "Architecture", null, null),
        ("Create/Window", "create", "Architecture", null, null),
        ("Create/Snap Point", "create", "Kit", "level", null),
        ("Create/", "create", "Shapes", null, null),
        ("UV/Make Starter Trims", "starter", "Starter content", null, null),
        ("UV/Trim", "surface", "Trims", "elements", null),
        ("UV/", "surface", "UVs", "elements", null),
        ("Material/Pick Under Mouse", "surface", "Materials", null, "Pick"), ("Material/", "surface", "Materials", "elements", "Apply Picked"),
        ("Paint/Make Starter Smart Materials", "starter", "Starter content", null, null),
        ("Paint/Edge Wear", "surface", "Wear", "level", null), ("Paint/Grime", "surface", "Wear", "level", null),
        ("Paint/Streaks", "surface", "Wear", "level", null), ("Paint/Bake Wear", "surface", "Wear", "level", "Bake"),
        ("Paint/Decal Brush", "surface", "Paint", null, "Decals"), ("Paint/Scatter Brush", "surface", "Paint", null, "Scatter"),
        ("Paint/Damage Brush", "surface", "Damage", null, "Brush"), ("Paint/Damage Selection", "surface", "Damage", "selection", "Selection"),
        ("Paint/Layer Brush", "surface", "Paint", "level", "Layers"), ("Paint/Erase Layers", "surface", "Paint", "level", "Erase"),
        ("Paint/", "surface", "Paint", "level", null),
        ("Modifier/", "object", "Modifiers", "level", null),
        ("Arrange/", "object", "Arrange", "selection", null),
        ("Wall/", "object", "Wall", "wall", null),
        ("Cut/", "object", "Cut", "cut", null),
        ("Spline/", "object", "Spline", "spline", null),
        ("Volume/", "world", "Volumes", "level", null),
        ("Navigation/", "world", "Navigation", null, null),
        ("View/", "view", "View", null, null),
        ("Help/", "starter", "Starter content", null, null),
    };

    private static void Place(EditorRegistry.MenuItem m)
    {
        foreach ((string prefix, string group, string section, string? needs, string? label) in s_places)
            if (m.Path.StartsWith(prefix, StringComparison.Ordinal))
            {
                m.Group ??= group;
                m.Section ??= section;
                m.Needs ??= needs;
                m.Label ??= label;
                return;
            }
    }

    // ---- the chord menus (leveltools.js): Shift+1–4 open the element modes' rings (Shift+5: world and
    // tools), then 1–6 while Shift is held pick a group, then its command. The slots are fixed, so a
    // command's keys don't move as commands are added. A slot: a command's path, or a group of its own
    // (More…; the page puts a list's sixth and later under More… too). The user's own layout (prefs
    // "chords") replaces a ring.
    private sealed record Chord(string Label, string Icon, object[] Slots);

    private static object[] Slots(params object[] slots) => slots;

    // The built-ins whose paths pass `take`, but not those already on the ring's group.
    private static object[] Rest(Func<string, bool> take, params object[] except)
    {
        var list = new List<object>();
        foreach (EditorRegistry.MenuItem m in EditorRegistry.Menus)
            if (m.Builtin && take(m.Path) && Array.IndexOf(except, m.Path) < 0)
                list.Add(m.Path);
        return list.ToArray();
    }

    private static (string Ring, string Label, string Icon, Chord[] Groups)[] Chords()
    {
        object[] shapes = Slots("Create/Box", "Create/Cylinder", "Create/Sphere", "Create/Cone", "Create/Torus");
        object[] splines = Slots("Create/Spline", "Create/Road", "Create/Kerb", "Create/Spline Wall", "Create/Rail", "Create/Spline Pipe");
        var tools = new List<object>();  // (editor code's own commands with an icon)
        foreach (EditorRegistry.MenuItem m in EditorRegistry.Menus)
            if (!m.Builtin && m.Icon != null)
                tools.Add(m.Path);
        Chord select(params object[] slots) => new("Select", "grow", slots);
        return new[]
        {
            ("1", "Object", "cube", new[]
            {
                new Chord("Shapes", "cube", [.. shapes, new Chord("More", "more",
                    Rest(p => Array.Exists(s_shapes, s => p == "Create/" + s.Label), shapes))]),
                new Chord("Build", "wall", Slots("Create/Wall", "Create/Room", "Create/Door", "Create/Window", "Create/Roof", new Chord("More", "more", Slots(
                    new Chord("Cuts", "cutBox", [.. Rest(p => p.StartsWith("Create/Cut ", StringComparison.Ordinal)), .. Rest(p => p.StartsWith("Cut/", StringComparison.Ordinal))]),
                    new Chord("Splines", "spline", splines),
                    "Create/Snap Point")))),
                new Chord("Modifiers", "mirror", Slots("Modifier/Mirror", "Modifier/Array", "Modifier/Solidify", "Modifier/Bevel", "Modifier/Bend",
                    new Chord("More", "more", Rest(p => p.StartsWith("Modifier/", StringComparison.Ordinal),
                        "Modifier/Mirror", "Modifier/Array", "Modifier/Solidify", "Modifier/Bevel", "Modifier/Bend")))),
                new Chord("Arrange", "align", Slots("Arrange/Drop to Ground", "Arrange/Snap Together", "Arrange/Distribute X", "Arrange/Distribute Z",
                    "Arrange/Group", "Arrange/Ungroup")),
                new Chord("Surface", "material", Slots("UV/Unwrap", "UV/Editor", "Material/Apply Picked", "Paint/Layer Brush", "Paint/Edge Wear", "Paint/Grime")),
                new Chord("Mesh", "weld", Slots("Mesh/Weld", "Mesh/Auto Smooth", "Mesh/Flip Normals", "Mesh/Conform Normals", "Mesh/Triangulate", "Mesh/Quadrangulate")),
            }),
            ("2", "Vertex", "vertex", new[]
            {
                new Chord("Geometry", "merge", Slots("Mesh/Merge", "Mesh/Connect", "Mesh/Fill", "Mesh/Weld", "Mesh/Delete")),
                select("Select/Grow", "Select/Shrink"),
            }),
            ("3", "Edge", "edge", new[]
            {
                new Chord("Geometry", "extrude", Slots("Mesh/Extrude", "Mesh/Bevel", "Mesh/Loop Cut", "Mesh/Connect", "Mesh/Fill", "Mesh/Delete")),
                select("Select/Loop", "Select/Ring", "Select/Grow", "Select/Shrink"),
            }),
            ("4", "Face", "face", new[]
            {
                new Chord("Geometry", "extrude", Slots("Mesh/Extrude", "Mesh/Inset", "Mesh/Subdivide", "Mesh/Detach", "Mesh/Merge Faces", "Mesh/Delete")),
                select("Select/Grow", "Select/Shrink", "Select/By Material", "Select/By Normal"),
                new Chord("UVs", "uvUnwrap", Slots("UV/Unwrap", "UV/Fit", "UV/Align to Edge", "UV/Rotate 90", "UV/Editor", new Chord("More", "more",
                    Slots("UV/Match Neighbours", "UV/Flip U", "UV/Flip V", "UV/Reset", "UV/Texture Lock", "UV/Local Projection")))),
                new Chord("Surface", "material", Slots("Material/Apply Picked", "UV/Trim Again", "UV/Trim Sheets", "Paint/Layer Brush", "Paint/Edge Wear", "Paint/Grime")),
                new Chord("Shading", "smooth", Slots("Mesh/Smooth Faces", "Mesh/Flat Faces", "Mesh/Flip Normals", "Mesh/Conform Normals", "Mesh/Triangulate",
                    "Mesh/Quadrangulate")),
            }),
            ("5", "World", "map", new[]
            {
                new Chord("Volumes", "trigger", Rest(p => p.StartsWith("Create/Volume/", StringComparison.Ordinal))),
                new Chord("Navigation", "navBake", Rest(p => p.StartsWith("Navigation/", StringComparison.Ordinal))),
                new Chord("View", "eye", Rest(p => p.StartsWith("View/", StringComparison.Ordinal))),
                new Chord("Tools", "code", tools.ToArray()),
            }),
        };
    }

    /// <summary>The chord menus' rings for the page (in the editor's tools description).</summary>
    internal static void WriteChords(Utf8JsonWriter w)
    {
        static void slots(Utf8JsonWriter w, object[] list)
        {
            w.WriteStartArray("slots");
            foreach (object s in list)
                if (s is Chord c)
                {
                    w.WriteStartObject();
                    w.WriteString("label", c.Label);
                    w.WriteString("icon", c.Icon);
                    slots(w, c.Slots);
                    w.WriteEndObject();
                }
                else
                    w.WriteStringValue((string)s);
            w.WriteEndArray();
        }
        w.WriteStartObject("chords");
        foreach ((string ring, string label, string icon, Chord[] groups) in Chords())
        {
            w.WriteStartObject(ring);
            w.WriteString("label", label);
            w.WriteString("icon", icon);
            slots(w, groups);
            w.WriteEndObject();
        }
        w.WriteEndObject();
    }

    private static List<CommandOption> ShapeOptions(string shape) => shape switch
    {
        "cylinder" or "cone" or "pipe" => new() { CommandOption.Whole("Sides", 16, 3, 64) },
        "sphere" or "torus" => new() { CommandOption.Whole("Sides", 16, 3, 64), CommandOption.Whole("Rings", 8, 2, 64) },
        "arch" => new() { CommandOption.Whole("Segments", 8, 1, 64) },
        "stairs" or "curvedStairs" => new() { CommandOption.Number("Step Height", 0.2, 0.05, 1, "m") },
        _ => new(),
    };

    private static List<CommandOption> WallOptions() =>
        new() { CommandOption.Number("Height", 3, 0.1, 50, "m"), CommandOption.Number("Thickness", 0.2, 0.01, 5, "m") };

    private static List<CommandOption> OpeningOptions(double width, double height, double sill) =>
        new()
        {
            CommandOption.Number("Width", width, 0.1, 20, "m"), CommandOption.Number("Height", height, 0.1, 20, "m"),
            CommandOption.Number("Sill", sill, 0, 20, "m"),
        };

    private static void Page(string type) => EditorApp.Send(type, _ => { });

    private static void AddModifier(string kind, string? variant = null) => EditorApp.Send("editor.level.modifier", w =>
    {
        w.WriteString("op", "add");
        w.WriteString("kind", kind);
        if (variant != null)
            w.WriteString(kind == "deform" ? "deform" : kind == "curve" ? "follow" : "mode", variant);
    });

    private static void SplineTool(string profile) => EditorApp.Send("editor.tool.set", w =>
    {
        w.WriteString("tool", "spline");
        w.WriteString("splineProfile", profile);
    });

    private static void Arrange(string op, int axis, int edge) => EditorApp.Send("editor.arrange", w =>
    {
        w.WriteString("op", op);
        w.WriteNumber("axis", axis);
        w.WriteNumber("edge", edge);
    });

    private static void LiveWear(string kind) => EditorApp.Send("editor.level.wear", w =>
    {
        w.WriteString("op", "add");
        w.WriteString("kind", kind);
    });

    private static void PaintTool(int mode) => EditorApp.Send("editor.tool.set", w =>
    {
        w.WriteString("tool", "paint");
        w.WriteNumber("paintMode", mode);
    });

    private static void Pair(Utf8JsonWriter w, string name, double a, double b)
    {
        w.WriteStartArray(name);
        w.WriteNumberValue(a);
        w.WriteNumberValue(b);
        w.WriteEndArray();
    }

    // A mesh operation through the editor on the selected level mesh's chosen elements.
    private static void Bridge(string op, Action<Utf8JsonWriter>? args = null) => EditorApp.Send("editor.level.op", w =>
    {
        w.WriteString("op", op);
        w.WriteStartObject("args");
        args?.Invoke(w);
        w.WriteEndObject();
    });

    private static readonly (string Name, string Label, string Icon)[] s_shapes =
    {
        ("box", "Box", "cube"), ("cylinder", "Cylinder", "cylinder"), ("cone", "Cone", "cone"), ("sphere", "Sphere", "sphere"),
        ("torus", "Torus", "torus"), ("pipe", "Pipe", "pipe"), ("prism", "Prism", "prism"), ("arch", "Arch", "arch"),
        ("stairs", "Stairs", "stairs"), ("curvedStairs", "Curved Stairs", "curvedStairs"), ("plane", "Plane", "plane"),
    };

    private static EditorRegistry.MenuItem Command(string path, string? key, string? modes, string icon, string? wheel, string help, Action run) =>
        EditorRegistry.Builtin(path, key, modes, icon, wheel, help, run);

    // ---- what a command acts on

    private sealed record Target(Entity Entity, PolyMesh Mesh, ElementMode Mode, List<ulong> Items);

    // The selected level mesh, its mode and chosen elements (the object mode, or nothing chosen yet: none).
    private static Target? Find()
    {
        var items = new List<ulong>();
        ElementMode mode = Elements.Get(out Entity entity, items);
        if (entity.Id == 0)
        {
            entity = Selection.Entity.Entity;
            items.Clear();
        }
        PolyMesh? mesh = entity.Id != 0 ? LevelMeshes.Get(entity) : null;
        if (mesh == null)
        {
            EditorApp.Notice("Select a level mesh first", "warning");
            return null;
        }
        return new Target(entity, mesh, mode, items);
    }

    // After a change: the result's selection when it is in the mode in use, else nothing chosen
    // (indices moved); results that keep the selection leave it.
    private static void Reselect(Target t, MeshOpResult r)
    {
        if (r.SelectionMode == ElementMode.Object || t.Mode == ElementMode.Object)
            return;
        Elements.Set(t.Entity, t.Mode, r.SelectionMode == t.Mode ? r.Selection : Array.Empty<ulong>());
    }

    // menu: its command, when it has settings (the last operation: changing them does it again).
    private static void Run(string name, string op, Action<Utf8JsonWriter>? args = null, string? menu = null)
    {
        if (Find() is Target t)
            RunOn(t, name, op, args, menu);
    }

    private static void RunOn(Target t, string name, string op, Action<Utf8JsonWriter>? args, string? menu)
    {
        MeshOpResult r = LevelOps.Run(op, t.Mesh, t.Mode, t.Items, args);
        if (!r.Ok)
        {
            EditorApp.Notice($"{name}: {r.Error}", "warning");
            return;
        }
        using (Undo.Scope(name))
            LevelMeshes.Set(t.Entity, r.Mesh!);
        Reselect(t, r);
        if (menu != null)
            LastOperation.Done(name, menu, null, null, 0, _ => RunOn(t, name, op, args, menu));
    }

    private static void Select(string op, Action<Utf8JsonWriter>? args = null)
    {
        if (Find() is not Target t || t.Mode == ElementMode.Object)
            return;
        MeshOpResult r = LevelOps.Run(op, t.Mesh, t.Mode, t.Items, args);
        if (r.Ok)
            Elements.Set(t.Entity, r.SelectionMode, r.Selection);
    }

    private static void Detach()
    {
        if (Find() is not Target t)
            return;
        MeshOpResult r = LevelOps.Run("detach", t.Mesh, t.Mode, t.Items);
        if (!r.Ok || r.Detached == null)
        {
            EditorApp.Notice($"Detach: {r.Error}", "warning");
            return;
        }
        using (Undo.Scope("Detach"))
        {
            LevelMeshes.Set(t.Entity, r.Mesh!);
            LevelMeshes.Create(r.Detached, t.Entity.Position, t.Entity.Rotation, t.Entity.Scale);
        }
    }

    private static void Modal(string name, Func<Target, string> op, string amountArg, float min, Func<Target, MeshModal>? make = null)
    {
        if (Find() is not Target t)
            return;
        if (t.Items.Count == 0)
        {
            EditorApp.Notice($"{name}: choose some elements first", "warning");
            return;
        }
        ModalOperation.Begin(make?.Invoke(t) ?? new MeshModal(name, op(t), amountArg, min, t));
    }

    // An operation whose amount follows the mouse: run again on the mesh it started with each time
    // the amount changes, shown live, one undo step when confirmed.
    private class MeshModal(string name, string op, string amountArg, float min, Target target) : ModalOperation
    {
        private MeshOpResult? _last;
        private float _lastAmount = float.NaN;

        public override string Name => name;
        internal override string? Menu => "Mesh/" + name;

        /// <summary>What its amount is called in the Level window's last operation.</summary>
        protected virtual string AmountName => char.ToUpperInvariant(amountArg[0]) + amountArg[1..];

        /// <summary>Its other arguments (after the amount).</summary>
        protected virtual void Arguments(Utf8JsonWriter w) { }

        /// <summary>Its other arguments changed: run again at the same amount.</summary>
        protected void Rerun() => _lastAmount = float.NaN;

        protected override void Apply(float amount)
        {
            amount = MathF.Max(amount, min);
            if (amount == _lastAmount)
                return;
            _lastAmount = amount;
            _last = LevelOps.Run(op, target.Mesh, target.Mode, target.Items, w =>
            {
                w.WriteNumber(amountArg, amount);
                Arguments(w);
            });
            LevelMeshes.SetLive(target.Entity, _last.Ok ? _last.Mesh! : target.Mesh);
        }

        protected override void OnConfirm(float amount)
        {
            Apply(amount);
            if (_last is not { Ok: true })
            {
                LevelMeshes.SetLive(target.Entity, target.Mesh);
                if (_last != null)
                    EditorApp.Notice($"{name}: {_last.Error}", "warning");
                return;
            }
            using (Undo.Scope(name))
                LevelMeshes.Commit(target.Entity, target.Mesh);
            Reselect(target, _last);
            LastOperation.Done(name, "Mesh/" + name, AmountName, "m", MathF.Max(amount, min), Again);
        }

        // Done again from the mesh it started with (the last operation changed).
        private void Again(float amount)
        {
            _lastAmount = float.NaN;
            OnConfirm(amount);
        }

        protected override void OnCancel() => LevelMeshes.SetLive(target.Entity, target.Mesh);
    }

    // Bevel: the mouse sets its width; the wheel its segments (1: a flat chamfer, more: round), C its
    // curve (round, flat, curved in). Both kept for the next bevel.
    private static readonly CommandOption s_bevelSegments = CommandOption.Whole("Segments", 1, 1, 32);
    private static readonly CommandOption s_bevelCurve = CommandOption.Choice("Curve", 0, "Round", "Flat", "Curved in");

    private static float BevelCurve => s_bevelCurve.Index switch { 1 => 0f, 2 => -1f, _ => 1f };

    private sealed class BevelModal(Target target) : MeshModal("Bevel", "bevel", "amount", 0.001f, target)
    {
        public override bool WantsWheel => true;
        protected override string AmountName => "Width";
        protected override string Details =>
            $" · {s_bevelSegments.Index} segment{(s_bevelSegments.Index == 1 ? "" : "s")}{(s_bevelSegments.Index > 1 ? " " + CurveName(BevelCurve) : "")}";
        protected override string KeyHints => "wheel segments · C round / flat / curved in · ";

        protected override void Arguments(Utf8JsonWriter w)
        {
            w.WriteNumber("segments", s_bevelSegments.Index);
            w.WriteNumber("curve", BevelCurve);
        }

        protected override bool OnKey(Key key, bool shift)
        {
            if (key != Key.C)
                return false;
            s_bevelCurve.Set((s_bevelCurve.Index + 1) % 3);  // round, flat, curved in
            if (s_bevelSegments.Index == 1)  // (a curve needs segments to show)
                s_bevelSegments.Set(4);
            Rerun();
            return true;
        }

        protected override void OnWheel(float notches)
        {
            int step = (int)MathF.Round(notches);
            s_bevelSegments.Set(s_bevelSegments.Index + (step != 0 ? step : notches > 0 ? 1 : -1));
            Rerun();
        }
    }

    internal static string CurveName(float curve) => curve > 0.5f ? "round" : curve < -0.5f ? "curved in" : "flat";

    // ---- walls and rooms, drawn in the view: corners on the ground (0.25 m; Shift: free), a post at
    // the mouse before the first one and the wall shown as it grows; right-click or Enter makes it (one
    // undo step), Esc leaves it.
    private sealed class WallTool(bool room, List<CommandOption> options) : ModalOperation
    {
        private readonly List<Vector2> _points = new();
        private Vector2 _mouse;
        private Entity _preview;
        private bool _onGround;

        public override string Name => _points.Count == 0 ? $"{(room ? "Room" : "Wall")}: click its first corner"
            : $"{(room ? "Room" : "Wall")}: {_points.Count} corner{(_points.Count == 1 ? "" : "s")}";
        internal override string? Menu => room ? "Create/Room" : "Create/Wall";
        protected override bool ShowsAmount => false;
        protected override bool RightClickFinishes => true;
        protected override string Keys => "click: a corner · Shift: off the grid · right-click / Enter: done · Esc: cancel";

        protected override void Apply(float amount)
        {
            ViewportInput i = SceneViewport.Input;
            _onGround = Ground(i, out Vector3 p);
            if (_onGround)
                _mouse = new Vector2(p.X, p.Z);
            if (!_onGround)
                return;
            if (_points.Count == 0)  // (before the first corner: a post where it would go)
            {
                float height = (float)options[0].Value, thickness = (float)options[1].Value;
                PolyMesh post = PolyMesh.Box(new Vector3(thickness, height, thickness));
                for (int k = 0; k < post.Positions.Count; ++k)
                    post.Positions[k] += new Vector3(_mouse.X, height * 0.5f, _mouse.Y);
                if (_preview.Id == 0)  // (an undo step, taken back when it is finished or left)
                    _preview = LevelMeshes.Create(post, Vector3.Zero);
                else
                    LevelMeshes.SetLive(_preview, post);
                return;
            }
            var path = new List<Vector2>(_points) { _mouse };
            if (Mesh(path, room && path.Count >= 3, options) is PolyMesh mesh)
                LevelMeshes.SetLive(_preview, mesh);
        }

        protected override bool OnClick()
        {
            if (!_onGround)
                return true;
            _points.Add(_mouse);
            if (_preview.Id == 0)  // (an undo step, taken back when it is finished or left)
                _preview = LevelMeshes.Create(PolyMesh.Box(new Vector3(0.01f)), Vector3.Zero);
            return true;
        }

        protected override void OnConfirm(float amount)
        {
            if (_preview.Id != 0)
                Undo.Perform();
            if (_points.Count >= (room ? 3 : 2))
                Make();
        }

        // The wall or room for the corners at the settings now (the last operation: again).
        private void Make()
        {
            EditorApp.Send("editor.level.wall.create", w =>
            {
                w.WriteStartArray("points");
                foreach (Vector2 p in _points)
                {
                    w.WriteStartArray();
                    w.WriteNumberValue(p.X);
                    w.WriteNumberValue(p.Y);
                    w.WriteEndArray();
                }
                w.WriteEndArray();
                w.WriteBoolean("closed", room);
                w.WriteBoolean("floor", room);
                w.WriteBoolean("ceiling", room);
                w.WriteNumber("height", options[0].Value);
                w.WriteNumber("thickness", options[1].Value);
            });
            LastOperation.Done(room ? "Room" : "Wall", room ? "Create/Room" : "Create/Wall", null, null, 0, _ => Make());
        }

        protected override void OnCancel()
        {
            if (_preview.Id != 0)
                Undo.Perform();
        }

        private static PolyMesh? Mesh(List<Vector2> path, bool closed, List<CommandOption> options)
        {
            MeshOpResult r = LevelOps.Run("wall", null, LevelOps.Args(ElementMode.Object, Array.Empty<ulong>(), w =>
            {
                w.WriteStartArray("points");
                foreach (Vector2 p in path)
                {
                    w.WriteStartArray();
                    w.WriteNumberValue(p.X);
                    w.WriteNumberValue(p.Y);
                    w.WriteEndArray();
                }
                w.WriteEndArray();
                w.WriteBoolean("closed", closed);
                w.WriteNumber("height", options[0].Value);
                w.WriteNumber("thickness", options[1].Value);
            }));
            return r.Ok ? r.Mesh : null;
        }

        private bool Ground(ViewportInput i, out Vector3 p)
        {
            p = default;
            if (i.RayDirection.Y > -1e-4f)
                return false;
            Vector3 hit = i.RayOrigin + i.RayDirection * (-i.RayOrigin.Y / i.RayDirection.Y);
            float snap(float v) => ShiftHeld ? v : MathF.Round(v / 0.25f) * 0.25f;
            p = new Vector3(snap(hit.X), 0, snap(hit.Z));
            return true;
        }
    }

    // ---- an opening: its outline follows the mouse over walls (where a click would cut it); a click on
    // a wall puts it there (where along the wall the click was).
    private sealed class OpeningTool(string name, List<CommandOption> options) : ModalOperation
    {
        private Vector2 _shown = new(-1);

        public override string Name => $"{name}: click a wall";
        internal override string? Menu => "Create/" + name;
        protected override bool ShowsAmount => false;
        protected override string Keys => "click: on a wall · right-click / Esc: cancel";

        private void Where(Utf8JsonWriter w)
        {
            ViewportInput i = SceneViewport.Input;
            w.WriteStartObject("at");
            w.WriteNumber("x", i.Size.X > 0 ? i.Mouse.X / i.Size.X : 0.5f);
            w.WriteNumber("y", i.Size.Y > 0 ? i.Mouse.Y / i.Size.Y : 0.5f);
            w.WriteEndObject();
            w.WriteNumber("width", options[0].Value);
            w.WriteNumber("height", options[1].Value);
            w.WriteNumber("sill", options[2].Value);
        }

        protected override void Apply(float amount)
        {
            if (SceneViewport.Input.Mouse == _shown)
                return;
            _shown = SceneViewport.Input.Mouse;
            EditorApp.Send("editor.level.opening.preview", Where);
        }

        protected override bool OnClick()
        {
            EditorApp.Send("editor.level.opening", w =>
            {
                Where(w);
                w.WriteString("name", name);
            });
            return false;  // (done)
        }

        private static void Hide() => EditorApp.Send("editor.level.opening.preview", w => w.WriteBoolean("off", true));
        protected override void OnConfirm(float amount) => Hide();
        protected override void OnCancel() => Hide();
    }

    // ---- shapes, drawn in the view

    // A block follows the mouse on the ground (y = 0) until a click puts the first corner, click the
    // opposite corner, then move up for the height (or type it) and click. Flat shapes (plane) and
    // round ones sized by their footprint (sphere, torus, arch) end at the second corner. Shift: no
    // 0.25 m snapping. Right-click: done with what is drawn (after the first corner).
    private sealed class ShapeTool(string shape, string label, List<CommandOption> options) : ModalOperation
    {
        private const float Grid = 0.25f;
        private int _step;  // 0: the first corner, 1: the second, 2: the height
        private Vector3 _a, _b;
        private float _height = 1f;
        private Entity _entity;
        private Vector3 _origin;  // where it was made: the live shape is moved within its mesh

        private bool Flat => shape is "plane" or "sphere" or "torus" or "arch";

        private double Option(string name, double fallback)
        {
            foreach (CommandOption o in options)
                if (o.Name == name)
                    return o.Value;
            return fallback;
        }

        public override string Name => _step switch
        {
            0 => $"{label}: click a corner on the ground",
            1 => $"{label}: click the opposite corner",
            _ => $"{label} height",
        };
        internal override string? Menu => "Create/" + label;
        protected override bool ShowsAmount => _step == 2;
        protected override bool RightClickFinishes => _step >= 1;
        protected override string Keys => _step == 0 ? "click: a corner on the ground · Shift: off the grid · Esc: cancel"
            : _step == 1 ? "click: the opposite corner · Shift: off the grid · right-click / Enter: done · Esc: cancel"
            : "move the mouse or type the height · Shift: off the grid · click / right-click / Enter: done · Esc: cancel";

        protected override void Apply(float amount)
        {
            ViewportInput i = SceneViewport.Input;
            if (_step < 2)
            {
                if (!Ground(i, out Vector3 p))
                    return;
                if (_step == 0)  // (before the first corner: a block where it would go)
                {
                    _a = p;
                    _b = p + new Vector3(Grid, 0, Grid);
                    if (_entity.Id == 0)  // (an undo step, undone again when it is finished or cancelled)
                        _entity = LevelMeshes.Create(Mesh(out _origin), _origin);
                }
                else
                    _b = p;
            }
            else
                _height = MathF.Max(Typed ? amount : Snapped(Height(i), ShiftHeld), 0.05f);
            if (_entity.Id != 0)
            {
                PolyMesh mesh = Mesh(out Vector3 at);
                for (int k = 0; k < mesh.Positions.Count; ++k)
                    mesh.Positions[k] += at - _origin;
                LevelMeshes.SetLive(_entity, mesh);
            }
        }

        protected override bool OnClick()
        {
            if (_step == 0)
            {
                _b = _a + new Vector3(Grid, 0, Grid);
                if (_entity.Id == 0)  // (the mouse wasn't over the ground before)
                    _entity = LevelMeshes.Create(Mesh(out _origin), _origin);
                _step = 1;
                return true;
            }
            if (_step == 1 && !Flat)
            {
                _height = MathF.Max(MathF.Min(MathF.Abs(_b.X - _a.X), MathF.Abs(_b.Z - _a.Z)), Grid);
                _step = 2;
                return true;
            }
            return false;
        }

        protected override void OnConfirm(float amount)
        {
            if (_entity.Id == 0)
                return;
            // One undo step for the finished shape: the one shown undone, made again.
            Undo.Perform();
            if (_step == 0)
                return;
            if (_step == 1 && !Flat)  // (finished at the footprint: as tall as it is wide)
                _height = MathF.Max(MathF.Min(MathF.Abs(_b.X - _a.X), MathF.Abs(_b.Z - _a.Z)), Grid);
            Make();
        }

        // The shape for the corners, height and settings now (the last operation: again).
        private void Make()
        {
            PolyMesh mesh = Mesh(out Vector3 at);
            LevelMeshes.Create(mesh, at);
            LastOperation.Done(label, $"Create/{label}", Flat ? null : "Height", "m", _height, h =>
            {
                _height = MathF.Max(h, 0.05f);
                Make();
            });
        }

        protected override void OnCancel()
        {
            if (_entity.Id != 0)
                Undo.Perform();
        }

        // The shape for the corners and height, and where its centre goes.
        private PolyMesh Mesh(out Vector3 at)
        {
            float w = MathF.Max(MathF.Abs(_b.X - _a.X), 0.05f), d = MathF.Max(MathF.Abs(_b.Z - _a.Z), 0.05f), h = _step < 2 && !Flat ? Grid : _height;
            float r = MathF.Max(w, d) * 0.5f;
            static void Size(Utf8JsonWriter j, float x, float y, float z)
            {
                j.WriteStartArray("size");
                j.WriteNumberValue(x);
                j.WriteNumberValue(y);
                j.WriteNumberValue(z);
                j.WriteEndArray();
            }
            int steps = Math.Clamp((int)MathF.Round(h / (float)Option("Step Height", 0.2)), 2, 64);
            PolyMesh? mesh = LevelOps.Shape(shape, j =>
            {
                foreach (CommandOption o in options)  // (sides, rings, segments)
                    if (o.Kind == "int")
                        j.WriteNumber(o.Name.ToLowerInvariant(), o.Index);
                switch (shape)
                {
                case "box":
                case "prism":
                    Size(j, w, h, d);
                    break;
                case "stairs":
                    Size(j, w, h, d);
                    j.WriteNumber("steps", steps);
                    break;
                case "plane":
                    j.WriteNumber("width", w);
                    j.WriteNumber("depth", d);
                    break;
                case "torus":
                    j.WriteNumber("radius", r * 0.75f);
                    j.WriteNumber("tube", r * 0.25f);
                    break;
                case "arch":
                    j.WriteNumber("radius", w * 0.5f);
                    j.WriteNumber("thickness", w * 0.15f);
                    j.WriteNumber("depth", d);
                    break;
                case "pipe":
                    j.WriteNumber("radius", r);
                    j.WriteNumber("thickness", MathF.Max(r * 0.2f, 0.02f));
                    j.WriteNumber("height", h);
                    break;
                case "curvedStairs":
                    j.WriteNumber("radius", r * 0.6f);
                    j.WriteNumber("width", r * 1.4f);
                    j.WriteNumber("height", h);
                    j.WriteNumber("steps", steps);
                    break;
                default:  // cylinder, cone, sphere
                    j.WriteNumber("radius", r);
                    j.WriteNumber("height", h);
                    break;
                }
            }) ?? PolyMesh.Box(Vector3.One);
            float top = float.MinValue;
            foreach (Vector3 p in mesh.Positions)
                top = MathF.Max(top, p.Y);
            at = new Vector3((_a.X + _b.X) * 0.5f, top, (_a.Z + _b.Z) * 0.5f);  // (centred: its bottom on the ground)
            return mesh;
        }

        private static float Snapped(float v, bool free) => free ? v : MathF.Round(v / Grid) * Grid;

        // Where the mouse's ray meets the ground (snapped).
        private bool Ground(ViewportInput i, out Vector3 p)
        {
            p = default;
            if (i.RayDirection.Y > -1e-4f)
                return false;
            float t = -i.RayOrigin.Y / i.RayDirection.Y;
            Vector3 hit = i.RayOrigin + i.RayDirection * t;
            p = new Vector3(Snapped(hit.X, ShiftHeld), 0, Snapped(hit.Z, ShiftHeld));
            return true;
        }

        // The height on the vertical line through the footprint's centre nearest the mouse's ray.
        private float Height(ViewportInput i)
        {
            Vector3 c = (_a + _b) * 0.5f, d = Vector3.Normalize(i.RayDirection), w0 = i.RayOrigin - c;
            float b = d.Y, denom = 1f - b * b;
            if (denom < 1e-5f)
                return _height;
            return (w0.Y - b * Vector3.Dot(d, w0)) / denom;
        }
    }
}
