package fixture;

import java.util.Comparator;
import java.util.function.Function;

public class Core {
    public Mapper mapper;
    public Rule rule;

    public Rule run() {
        return Rule.bind(mapper, rule);
    }

    public Comparator jdk(Function mapper, Comparator comparator) {
        return Comparator.comparing(mapper, comparator);
    }
}
