import kotlin.jvm.functions.Function1;
import fixture.Trace;
public final class LocalStaticPrefixProbe implements Function1<Object,Object> {
    private final int tag;
    public LocalStaticPrefixProbe(int tag){this.tag=tag;}
    public Object invoke(Object value){return Trace.accept(tag,value);}
}
