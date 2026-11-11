import fixture.*;
public final class ConditionalPackedCaller {
    public static Target call(Creator creator,Object left,Object right,float first,float second,int shift,long mask,int low,int high) {
        return creator.make(left,right,first,second,shift,mask,low,high);
    }
}
