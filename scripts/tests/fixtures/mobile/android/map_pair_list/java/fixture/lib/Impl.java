package fixture.lib;
import fixture.Pair;
import fixture.State;
import java.util.List;
import java.util.Map;
class Impl {
    public static final <K, V> List<Pair<K, V>> convert(Map<? extends K, ? extends V> input) {
        State.events = State.events * 5 + 1;
        State.savedInput = input;
        int ignored = input.size();
        State.marker = State.probe.observe(input);
        if (State.failure != null) throw State.failure;
        State.savedResult = State.replacement;
        State.events = State.events * 5 + 2;
        return State.replacement;
    }
}
