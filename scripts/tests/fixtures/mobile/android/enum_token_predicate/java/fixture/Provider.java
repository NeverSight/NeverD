package fixture;
public class Provider {
    public static <Choice extends Enum<Choice>> boolean test(Class<Choice> type, String key) {
        Trace.type=type;Trace.key=key;Trace.hit(1);
        Trace.action.run();Trace.hit(2);
        return Trace.result;
    }
}
