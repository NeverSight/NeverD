package fixture;
import java.util.function.Function;
public class Provider {
    public static <Input,Output> Output[] map(Input[] source, Class<Output> type, Function<? super Input,? extends Output> mapper) {
        Trace.source=source;Trace.type=type;Trace.mapper=mapper;Trace.hit(1);
        Trace.action.run();Trace.hit(2);
        if(Trace.apply) Trace.mapped=mapper.apply(source[Trace.index]);
        Trace.hit(3);
        return (Output[])Trace.result;
    }
}
