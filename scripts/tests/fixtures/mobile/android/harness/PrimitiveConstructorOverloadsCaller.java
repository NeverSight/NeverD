import fixture.*;
public class PrimitiveConstructorOverloadsCaller {
    public static Core<String, Integer> create(int width, Object input) {
        return width == 0 ? new Core<String, Integer>((int) Trace.read(input))
                          : new Core<String, Integer>(Trace.read(input));
    }
}
