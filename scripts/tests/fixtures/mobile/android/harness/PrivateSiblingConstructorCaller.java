import fixture.Core;
import fixture.Probe;
import java.io.IOException;
import java.util.List;

public final class PrivateSiblingConstructorCaller {
    public static Object call(Object context, List<?> values, Probe probe) throws IOException {
        return Core.create(context,values,probe);
    }
}
