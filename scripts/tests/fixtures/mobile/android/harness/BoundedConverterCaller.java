import fixture.*;
public final class BoundedConverterCaller {
    public static Bound call(Converter converter,Object value) { return Provider.access(converter,value); }
}
