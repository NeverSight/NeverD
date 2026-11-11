package fixture;

public final class Provider {
    private Provider() {}
    public interface Pair<A,B> { B apply(A owner,B seed); }
    public interface Single<B> { B apply(B value); }
    @SuppressWarnings("unchecked")
    public static <A,B> B read(A owner,Pair<A,B> pair,Single<B> single,B seed,Context context,int direction) {
        Trace.hit(1,owner,pair,single,seed,context,null,direction,0L,false,false);
        B paired=pair.apply(owner,seed);
        Trace.hit(2,owner,pair,single,seed,context,paired,direction,0L,false,false);
        B mapped=single.apply(paired);
        B result=(B)context.reply;
        Trace.hit(3,owner,pair,single,seed,context,mapped,direction,0L,false,false);
        return result;
    }
    @SuppressWarnings("unchecked")
    public static <A,B> B relative(A owner,Pair<A,B> pair,Single<B> single,B seed,long stamp,int direction,boolean forward,boolean wrap) {
        Trace.hit(1,owner,pair,single,seed,null,null,direction,stamp,forward,wrap);
        B paired=pair.apply(owner,seed);
        Trace.hit(2,owner,pair,single,seed,null,paired,direction,stamp,forward,wrap);
        B mapped=single.apply(paired);
        B result=(B)Trace.reply;
        Trace.hit(3,owner,pair,single,seed,null,mapped,direction,stamp,forward,wrap);
        return result;
    }
}
