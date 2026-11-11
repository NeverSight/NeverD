import fixture.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class LongScalarConsumerCaller {
    public static void write(Consumer self, Token token, Object value) { self.write(token, value); }
    public static Object read(Producer self, Token token) { return self.read(token); }
    public static Token meta(Api self) { return self.meta(); }
}
