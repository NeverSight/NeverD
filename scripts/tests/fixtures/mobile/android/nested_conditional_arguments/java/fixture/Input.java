package fixture;
public class Input {
    public Object first,second;
    public int firstCalls,secondCalls;
    public Input(Object first,Object second) { this.first=first;this.second=second; }
    public Object first() { firstCalls++;Trace.hit(3);return first; }
    public Object second() { secondCalls++;Trace.hit(4);return second; }
}
