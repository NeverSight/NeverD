package fixture;
public final class Host {
    private Host() {}
    public static int run(int index) { Trace.hit(0,null);int value=Table.read(index);Trace.hit(9,Table.mapping);return value; }
    public static void ignore(int index) { Trace.hit(0,null);Table.read(index);Trace.hit(9,Table.mapping); }
    public static final class Table {
        private Table() {}
        public static final int[] mapping;
        static { Trace.hit(1,null);int[] result=(int[])Seed.make();Trace.hit(2,result);mapping=result;Trace.hit(3,result); }
        public static int read(int index) { Trace.hit(4,mapping);int value=mapping[index];Trace.hit(5,mapping);return value; }
    }
}
