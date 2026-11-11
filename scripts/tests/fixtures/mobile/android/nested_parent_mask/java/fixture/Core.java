package fixture;
final class Core<T> extends Parent<Local<T>> {
    public final int count;
    public final long bits;
    public final Sink<? super T> callback;
    @SuppressWarnings("unchecked")
    public Core(int count,long bits,Sink<? super T> callback) {
        Trace.partial=this;Trace.hit(6);this.count=count;this.bits=bits;this.callback=callback;
        Trace.hit(7);callback.accept((T)Trace.token);Trace.hit(9);
    }
    @SuppressWarnings("unchecked")
    public Core(int count,long bits,Sink callback,int mask,kotlin.jvm.internal.DefaultConstructorMarker marker) {
        this(count,(mask&2)!=0?Defaults.defaultBits():bits,(Sink<? super T>)((mask&4)!=0?Defaults.CB:callback));
    }
}
