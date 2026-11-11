package fixture;

public final class Owner {
    public static final View PROTOTYPE = new View() {
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

    public static View get() { return PROTOTYPE; }
}
