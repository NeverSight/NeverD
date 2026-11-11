package fixture;
public final class Trace {
    private Trace() {}
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static int[] provided,late;
    public static void hit(int phase,int[] value) {
        events=events*10+phase;
        if(phase==2){if(value!=provided)throw new AssertionError("owned table result identity");Seed.next=late;}
        if(phase==4&&value!=null)value[0]+=10;
        if(phase==5&&value!=null)value[0]+=100;
        if(phase==9&&value!=null)value[0]+=1000;
        if(phase==failAt)throw failure;
    }
}
