package fixture;
public final class Core {
    private Core() {}
    public static Object run(Object type, Object value) {
        Token token = (Token)type; Carrier input = (Carrier)value;
        Trace.hit(0, token, input, null, null);
        Carrier result = Provider.make(token, input);
        Trace.hit(5, token, input, null, result);
        return result;
    }
    public static String checked(Object type, Object value) {
        Token token = (Token)type; Carrier input = (Carrier)value;
        Trace.hit(0, token, input, null, null);
        Carrier result = Provider.make(token, input);
        Trace.hit(5, token, input, null, result);
        return ((String[])result.read())[0];
    }
}
