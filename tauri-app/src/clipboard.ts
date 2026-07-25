/**
 * Copy text to the system clipboard.
 *
 * `navigator.clipboard` is only available in a secure context, and WebView2
 * does not always treat the `http://tauri.localhost` origin as one — so fall
 * back to the old hidden-textarea trick, which works in every webview.
 *
 * Returns true if either path reported success.
 */
export async function copyText(text: string): Promise<boolean> {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    // Fall through to the execCommand path.
  }

  try {
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.setAttribute("readonly", "");
    ta.style.position = "fixed";
    ta.style.opacity = "0";
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand("copy");
    document.body.removeChild(ta);
    return ok;
  } catch {
    return false;
  }
}
