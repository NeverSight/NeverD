package fixture;
import fixture.lib.Facade;
import java.util.List;
import java.util.Map;
public final class Core {
    private Core() {}
    public static List call(Map input) {
        State.events = State.events * 5 + 3;
        List result = Facade.convert(input);
        State.events = State.events * 5 + 4;
        return result;
    }
    public static <R, S> List<Pair<R, S>> typed(Map<? extends R, ? extends S> input) {
        State.events = State.events * 5 + 3;
        List<Pair<R, S>> result = Facade.convert(input);
        State.events = State.events * 5 + 4;
        return result;
    }
    public static void discard(Map input) { call(input); }
    public static Pair head(Map input) {
        List result = call(input);
        State.events = State.events * 5 + 6;
        return (Pair) result.get(0);
    }
}
