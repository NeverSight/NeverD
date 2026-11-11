package fixture;
public class Core<K,V> extends Base<K,V> {
    public final Object token;
    public Object value,back;
    public Core(Object token,Object value) { Trace.created++;this.token=token;this.value=value;this.back=this; }
    @SuppressWarnings("unchecked")
    public Core<K,V> copy() {
        try {
            Core<K,V> copy=(Core<K,V>)super.clone();
            Trace.hit(1);Trace.partial=copy;copy.value=Trace.result;Trace.hit(2);
            return copy;
        } catch(CloneNotSupportedException failure) { throw new InternalError(failure); }
    }
}
