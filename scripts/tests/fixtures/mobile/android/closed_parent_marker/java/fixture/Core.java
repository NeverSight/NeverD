package fixture;
import api.Function;
import kotlin.jvm.internal.DefaultConstructorMarker;
final class Core extends Parent<LocalToken> {
    public final long bits;
    public final Context context;
    public final float weight;
    public final Token token;
    public final Function<? super Context,Unit> callback;
    private Core(long bits,Context context,float weight,Token token,Function<? super Context,Unit> callback) {
        super();Trace.core=this;Trace.hit(1);
        this.bits=bits;this.context=context;this.weight=weight;this.token=token;this.callback=callback;Trace.hit(2);
        Trace.unit=callback.invoke(context);Trace.hit(3);Trace.done=true;Trace.hit(4);
    }
    public Core(long bits,Context context,float weight,Token token,Function callback,DefaultConstructorMarker marker) {
        this(bits,context,weight,token,(Function<? super Context,Unit>)callback);
    }
}
