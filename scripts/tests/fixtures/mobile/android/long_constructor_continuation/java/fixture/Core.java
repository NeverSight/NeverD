package fixture;

public class Core {
    public final Object token;
    public int state;
    public Object tail;

    public Core(Provider provider, boolean chooseTail) {
        this(Provider.seed(provider));
        Trace.hit(3, this, provider, -1);
        this.state = Trace.step(0, this, provider);
        this.state = Trace.step(1, this, provider);
        this.state = Trace.step(2, this, provider);
        this.state = Trace.step(3, this, provider);
        this.state = Trace.step(4, this, provider);
        this.state = Trace.step(5, this, provider);
        this.state = Trace.step(6, this, provider);
        this.state = Trace.step(7, this, provider);
        this.state = Trace.step(8, this, provider);
        this.state = Trace.step(9, this, provider);
        this.state = Trace.step(10, this, provider);
        this.state = Trace.step(11, this, provider);
        this.state = Trace.step(12, this, provider);
        this.state = Trace.step(13, this, provider);
        this.state = Trace.step(14, this, provider);
        this.state = Trace.step(15, this, provider);
        this.state = Trace.step(16, this, provider);
        this.state = Trace.step(17, this, provider);
        this.state = Trace.step(18, this, provider);
        this.state = Trace.step(19, this, provider);
        this.state = Trace.step(20, this, provider);
        this.state = Trace.step(21, this, provider);
        this.state = Trace.step(22, this, provider);
        this.state = Trace.step(23, this, provider);
        this.state = Trace.step(24, this, provider);
        this.state = Trace.step(25, this, provider);
        this.state = Trace.step(26, this, provider);
        this.state = Trace.step(27, this, provider);
        this.state = Trace.step(28, this, provider);
        this.state = Trace.step(29, this, provider);
        this.state = Trace.step(30, this, provider);
        this.state = Trace.step(31, this, provider);
        this.state = Trace.step(32, this, provider);
        this.state = Trace.step(33, this, provider);
        this.state = Trace.step(34, this, provider);
        this.state = Trace.step(35, this, provider);
        this.state = Trace.step(36, this, provider);
        this.state = Trace.step(37, this, provider);
        this.state = Trace.step(38, this, provider);
        this.state = Trace.step(39, this, provider);
        this.state = Trace.step(40, this, provider);
        this.state = Trace.step(41, this, provider);
        this.state = Trace.step(42, this, provider);
        this.state = Trace.step(43, this, provider);
        this.state = Trace.step(44, this, provider);
        this.state = Trace.step(45, this, provider);
        this.state = Trace.step(46, this, provider);
        this.state = Trace.step(47, this, provider);
        this.state = Trace.step(48, this, provider);
        this.state = Trace.step(49, this, provider);
        this.state = Trace.step(50, this, provider);
        this.state = Trace.step(51, this, provider);
        this.state = Trace.step(52, this, provider);
        this.state = Trace.step(53, this, provider);
        this.state = Trace.step(54, this, provider);
        this.state = Trace.step(55, this, provider);
        this.state = Trace.step(56, this, provider);
        this.state = Trace.step(57, this, provider);
        this.state = Trace.step(58, this, provider);
        this.state = Trace.step(59, this, provider);
        this.state = Trace.step(60, this, provider);
        this.state = Trace.step(61, this, provider);
        this.state = Trace.step(62, this, provider);
        this.state = Trace.step(63, this, provider);
        this.state = Trace.step(64, this, provider);
        this.state = Trace.step(65, this, provider);
        this.state = Trace.step(66, this, provider);
        this.state = Trace.step(67, this, provider);
        this.state = Trace.step(68, this, provider);
        this.state = Trace.step(69, this, provider);
        this.state = Trace.step(70, this, provider);
        this.state = Trace.step(71, this, provider);
        if (chooseTail) {
            this.tail = Provider.tail(provider);
        } else {
            this.tail = token;
        }
        Trace.hit(6, this, provider, -1);
    }

    public Core(Object token) {
        Trace.hit(1, this, null, -1);
        this.token = token;
        Trace.hit(2, this, null, -1);
    }

    public String kind() { return "base"; }
}
