package fixture;
public class Producer implements CountView<Object> {
    public final int length() { return getCount(); }
    public int getCount() {
        Trace.seen=this; Trace.calls++;
        int result=Trace.value; Trace.hit(3);
        StackTraceElement[] frames=new Throwable().getStackTrace();
        if(Trace.calls==1)Trace.firstFrames=frames;
        Trace.lastFrames=frames; Trace.value=Trace.next; Trace.hit(4);
        return result;
    }
}
