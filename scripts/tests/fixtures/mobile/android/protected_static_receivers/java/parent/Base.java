package parent;
import api.Bound;
public abstract class Base implements Bound {
    public int calls, fail;
    public Bound seen, replacement;
    public RuntimeException failure;
    @SuppressWarnings("unchecked")
    protected final <T extends Bound> T choose(T value) {
        ++calls;
        if(fail==1)throw failure;
        seen=value;
        if(fail==2)throw failure;
        return (T)replacement;
    }
}
