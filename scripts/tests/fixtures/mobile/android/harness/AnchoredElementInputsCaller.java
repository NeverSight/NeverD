import fixture.*;
public final class AnchoredElementInputsCaller {
    public static Object call(Cell<?> cell,float offset,float limit,Fn<?,?> callback,Producer<?> producer,boolean checked) {
        return checked?Owner.text(cell,offset,limit,callback,producer):Owner.call(cell,offset,limit,callback,producer);
    }
}
