package fixture;
public final class Core {
    static { Trace.hit(9); }
    public final int[] left;
    public final int[] right;
    public Core(int[] left,int[] right,Hook hook) {
        Trace.partial=this; Trace.hit(2);
        this.left=left; Trace.hit(3);
        left[0]+=Trace.delta; Trace.hit(4);
        hook.apply(this,right); Trace.hit(5);
        this.right=right; Trace.hit(6);
    }
    public Core(int first,int second,Hook hook) {
        int value=Trace.seed(first);
        int[] left=new int[]{value,second};
        int[] right=new int[]{second,first};
        this(left,right,hook);
        left[1]=Trace.post; Trace.hit(7);
    }
}
