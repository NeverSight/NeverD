import fixture.Shape;
import fixture.Trace;
public final class LongConstructorPrefixProbe implements Shape {
    public int width(){Trace.step(2);return Trace.width;}
    public int height(){Trace.step(3);int result=Trace.height;Trace.width=~Trace.width;return result;}
}
