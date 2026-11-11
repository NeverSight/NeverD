import fixture.*;
public class GenericArrayTransfersCaller {
    public static Object[] move(Object[] source, Object[] destination, int offset, int begin, int end) {
        return Core.move(source, destination, offset, begin, end);
    }
}
