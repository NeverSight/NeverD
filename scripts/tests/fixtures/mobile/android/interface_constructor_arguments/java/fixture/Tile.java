package fixture;

public class Tile implements Api {
    public final boolean flag;
    public final int number;
    public final long ticks;
    public Object mark;

    static {
        Trace.events = Trace.events * 10 + 1;
        Trace.stage = 1;
        Trace.action.run();
        Trace.stage = 2;
    }

    public Tile(boolean flag, int number, long ticks) {
        Trace.constructors++;
        Trace.events = Trace.events * 10 + 2;
        Trace.escaped = this;
        Trace.flag = flag;
        Trace.number = number;
        Trace.ticks = ticks;
        Trace.stage = 10 + Trace.constructors;
        Trace.action.run();
        this.flag = flag;
        this.number = number;
        this.ticks = ticks;
        Trace.completed++;
    }

    public Tile(float flag, double ticks) {
        int number = Float.floatToRawIntBits(flag);
        long bits = Double.doubleToRawLongBits(ticks);
        Trace.constructors++;
        Trace.events = Trace.events * 10 + 2;
        Trace.escaped = this;
        Trace.flag = false;
        Trace.number = number;
        Trace.ticks = bits;
        Trace.stage = 10 + Trace.constructors;
        Trace.action.run();
        this.flag = false;
        this.number = number;
        this.ticks = bits;
        Trace.completed++;
    }

    public Tile(int number) {
        Trace.alternates++;
        this.flag = false;
        this.number = number;
        this.ticks = 0;
    }
}
