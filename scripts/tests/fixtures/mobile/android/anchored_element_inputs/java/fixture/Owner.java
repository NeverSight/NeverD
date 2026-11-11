package fixture;
public final class Owner {
    static {Trace.hit(1);}
    private static <T> T select(Cell<T> cell, float offset, float limit,
            Fn<? super Label,Label> callback, Producer<Label> producer) {
        Trace.cell=cell;Trace.offset=offset;Trace.limit=limit;Trace.hit(2);
        Label produced=producer.get();Trace.produced=produced;Trace.hit(4);
        Label mapped=callback.apply(produced);Trace.mapped=mapped;Trace.hit(6);
        T chosen=offset<limit?cell.first:cell.second;Trace.selected=chosen;Trace.hit(7);
        return Trace.override?(T)Trace.result:chosen;
    }
    public static Object call(Cell<?> cell,float offset,float limit,Fn<?,?> callback,Producer<?> producer) {
        return select((Cell)cell,offset,limit,(Fn)callback,(Producer)producer);
    }
    public static String text(Cell<?> cell,float offset,float limit,Fn<?,?> callback,Producer<?> producer) {
        return (String)select((Cell)cell,offset,limit,(Fn)callback,(Producer)producer);
    }
}
