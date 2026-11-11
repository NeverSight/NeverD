package fixture;
public abstract class Provider implements java.io.Serializable {
    public int calls;
    public static <E> E read(Provider receiver,Closed<Context,Ticket> setup,Context context,Producer<E> producer) throws java.io.IOException {
        return receiver.readInstance(setup,context,producer);
    }
    public <E> E readInstance(Closed<Context,Ticket> setup,Context context,Producer<E> producer) throws java.io.IOException {
        Trace.hit(3); calls++;
        Trace.setup=setup; Trace.context=context; Trace.producer=producer;
        Ticket ticket=setup.apply(context); Trace.ticket=ticket; Trace.hit(9);
        context.action.run(); Trace.hit(4);
        if(context.checked!=null) throw context.checked;
        E value=producer.produce(ticket);
        Trace.hit(5); Trace.produced=value; Trace.providerAfter.run(); Trace.hit(6);
        return value;
    }
}
