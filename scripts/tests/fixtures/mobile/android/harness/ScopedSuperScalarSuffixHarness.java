package fixture;
import java.util.ArrayList;
import java.util.List;

public final class ScopedSuperScalarSuffixHarness {
    static final RuntimeException FAILURE=new IllegalArgumentException("owned-failure");
    static final Object SEED=new Object(),RESULT=new Object(),LATE=new Object();
    static int route,mode;
    static float number;
    static Object seed,expectedResult;
    static Carrier<Object> property;
    static Base<?> partial;
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean value) {
        if(!value)throw new AssertionError("owned scoped super scalar suffix mismatch");
    }
    static final class Reply implements Carrier<Object> {
        Object reply;
        Reply(Object result) {reply=result;}
        public Object apply(Object value) {
            check(value==seed);events.add(5);
            if(mode==6)throw FAILURE;
            return reply;
        }
    }
    static final class Direct extends Base<Child> {
        <A> Direct(A value,Carrier<A> callback,float number) {
            super(value,callback);auxiliary=number;stage=1;Trace.hit(4,this,value,callback,snapshot);
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase,Object parent,Object value,Object callback,Object result) {
            check(value==seed&&callback==property);
            Base<?> current=(Base<?>)parent;
            if(phase==1)partial=current;
            check(partial==current);
            check(current.seed==(phase==1?null:seed));
            check(current.property==(phase==1?null:property));
            check(current.snapshot==(phase<3?null:expectedResult));
            check(current.stage==(phase==4?1:0));
            check(Float.floatToRawIntBits(current.auxiliary)==(phase==4?Float.floatToRawIntBits(number):0));
            check(result==(phase<3?null:expectedResult));
            events.add(phase);
            if(mode==2&&phase==1||mode==3&&phase==2||mode==4&&phase==3||mode==5&&phase==4)throw FAILURE;
            if(phase==3&&property!=null)((Reply)property).reply=LATE;
        }
    }
    static void attempt() {
        events.clear();partial=null;seed=mode==1?null:SEED;
        number=Float.intBitsToFloat(mode==8?0x7fc01234:mode==9?0x80000000:0x3f812345);
        expectedResult=mode==8?null:mode==9?seed:RESULT;
        property=mode==7?null:new Reply(expectedResult);
        Trace.hook=new Observer();Throwable failure=null;Base<?> made=null;
        try {
            if(route==1)made=new Direct(seed,property,number);
            else if(route==2)new Child(seed,property,number);
            else made=new Child(seed,property,number);
        } catch(Throwable caught) {failure=caught;}
        int[] expected={1,2,5,3,4};boolean exact=false;Class<?> kind=null;
        if(mode==2) {expected=new int[]{1};exact=true;}
        else if(mode==3) {expected=new int[]{1,2};exact=true;}
        else if(mode==4) {expected=new int[]{1,2,5,3};exact=true;}
        else if(mode==5) {exact=true;}
        else if(mode==6) {expected=new int[]{1,2,5};exact=true;}
        else if(mode==7) {expected=new int[]{1,2};kind=NullPointerException.class;}
        List<Integer> wanted=new ArrayList<Integer>();for(int event:expected)wanted.add(event);
        check(events.equals(wanted));
        if(exact)check(failure==FAILURE);
        else if(kind!=null)check(failure!=null&&failure.getClass()==kind);
        else {check(failure==null);check(route==2?made==null:made==partial);check(partial.snapshot==expectedResult);check(partial.stage==1);}
        Trace.hook=null;
    }
    public static void main(String[] args) {
        route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);attempt();attempt();
        System.out.println("attempts=2,scoped-super-scalar-suffix=matched");
    }
}
