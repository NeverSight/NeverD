package fixture;

public final class Owner {
    public static final View PROTOTYPE = new View() {
        { Owner.initial = this.zeroI; Owner.prepare(this, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20); }

        public boolean zeroZ;
        public byte zeroB;
        public char zeroC;
        public short zeroS;
        public int zeroI;
        public long zeroJ;
        public float zeroF;
        public double zeroD;
        public Object zeroO;
        public int[] zeroA;

        public boolean readZ() { return zeroZ; }
        public byte readB() { return zeroB; }
        public char readC() { return zeroC; }
        public short readS() { return zeroS; }
        public int readI() { return zeroI; }
        public long readJ() { return zeroJ; }
        public float readF() { return zeroF; }
        public double readD() { return zeroD; }
        public Object readO() { return zeroO; }
        public int[] readA() { return zeroA; }
    };

    public static View observed;
    public static int calls;
    public static int sum;
    public static int initial;
    public static Runnable hook;

    public static void prepare(View value, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11, int a12, int a13, int a14, int a15, int a16, int a17, int a18, int a19, int a20) {
        observed = value;
        calls++;
        sum = a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20;
        if (hook != null) hook.run();
    }

    public static View get() { return PROTOTYPE; }
}
