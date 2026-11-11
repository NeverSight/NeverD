# Protected bounded self receiver

A nongeneric abstract parent in another package declares a protected final
<T extends Bound> T choose(T input), where Bound is a public interface. A child
calls the method on this. The method records its receiver/input, invokes input
behavior, and returns an independently selected bound object or null. The result
is intentionally different from the input.

Frozen direct, compiled-caller, reflection and method-handle observers check
receiver and result identity, callback effects, null input/receiver/result,
callback updates to the selected output, publication timing, and exact exception
identity at each stage. They run twice per case. Independent wrong programs return
the input, use a different receiver, skip the callback, or delay result publication.
The SDK variant imports the original parent and interface declarations. All
sources are first-party Java 8 and contain no private application information.
