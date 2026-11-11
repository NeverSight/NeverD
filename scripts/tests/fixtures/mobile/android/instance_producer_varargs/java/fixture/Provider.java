package fixture;
public abstract class Provider implements java.io.Serializable {
    public int calls;
    public <E> E read(Context context,Producer<E> producer,String... labels) throws java.io.IOException {
        Trace.hit(3); calls++;
        Trace.labels=labels; Trace.context=context; Trace.producer=producer;
        context.action.run(); Trace.hit(4);
        if(labels!=null && labels.length!=0) { Trace.beforeLabel=labels[0]; labels[0]=Trace.replacement; }
        if(context.checked!=null) throw context.checked;
        E value=producer.produce(context);
        Trace.hit(5); Trace.produced=value; Trace.providerAfter.run(); Trace.hit(6);
        return value;
    }
}
