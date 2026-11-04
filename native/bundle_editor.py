"""Bundle the editor modules; UI artwork is packaged as native APK assets."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent
def bundle():
    modules = [("createSavedScripts", "saved_scripts"),
               ("createEditorSettings", "editor_settings"),
               ("createExecutionService", "execution_service")]
    return "".join("local function " + name + "(context)\n" +
                    (ROOT.parent / "scripts" / (file + ".luau")).read_text() + "\nend\n"
                    for name, file in modules) + (ROOT.parent / "scripts/editor.luau").read_text()

if __name__ == "__main__":
    (ROOT / "editor_bundled.luau").write_text(bundle())
