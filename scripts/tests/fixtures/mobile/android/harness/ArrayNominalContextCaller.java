import fixture.*;
public final class ArrayNominalContextCaller {
    public static <E> String call(E[] values,CharSequence delimiter){return Core.call(values,delimiter);}
    public static String checked(Object values,CharSequence delimiter){return Core.checked(values,delimiter);}
}
