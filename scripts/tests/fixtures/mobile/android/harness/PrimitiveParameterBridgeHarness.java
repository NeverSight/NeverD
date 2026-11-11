package fixture;
import java.util.ArrayList;
import java.util.List;

public final class PrimitiveParameterBridgeHarness {
    static final RuntimeException FAILURE=new IllegalStateException("owned-failure");
    static final int INITIAL=0x3ea12345,LATE=0x40c12345;
    static int route,mode,bits,initial;
    static Value value;
    static Object input;
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean condition) {
        if(!condition)throw new AssertionError("owned primitive parameter bridge mismatch");
    }
    static int raw(float number) { return Float.floatToRawIntBits(number); }
    static final class Observer extends Trace {
        public void observe(int phase,Object seen,float number) {
            check(seen==input);
            int expected=phase==2||phase==3&&(route>=2)||phase==4&&(route>=2)?bits:phase==3||phase==4?initial:0;
            check(raw(number)==expected);
            events.add(phase);
            if(mode==5&&(phase==1||phase==2)||mode==6&&phase==3||mode==7&&phase==4)throw FAILURE;
            if(phase==3) {check(raw(value.number)==(route<2?initial:bits));value.number=Float.intBitsToFloat(LATE);}
        }
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    static void attempt() {
        events.clear();value=new Value();
        input=mode==3?null:mode==4?new Object():value;
        bits=mode==8?0x7fc01234:mode==9?0x80000000:mode==10?0x7f800000:mode==11?0xff800000:0x3f812345;
        initial=mode>=8?bits:INITIAL;value.number=Float.intBitsToFloat(initial);
        float number=Float.intBitsToFloat(bits);
        Object receiver=mode==1?null:mode==2?new Object():Host.shared();
        Trace.hook=new Observer();Throwable failure=null;float answer=0.0f;
        try {
            if(route==0)answer=Core.get(receiver,input);
            else if(route==2)Core.put(receiver,input,number);
            else {
                Trace.hit(0,input,0.0f);
                if(route==1) {answer=((Base)receiver).get(input);Trace.hit(4,input,answer);}
                else {((Base)receiver).put(input,number);Trace.hit(4,input,number);}
            }
        } catch(Throwable caught) {failure=caught;}
        int target=route<2?1:2;
        int[] expected={0,target,3,4};Class<?> kind=null;boolean exact=false;
        if(mode==1) {expected=new int[]{0};kind=NullPointerException.class;}
        else if(mode==2||mode==4) {expected=new int[]{0};kind=ClassCastException.class;}
        else if(mode==3) {expected=new int[]{0,target};kind=NullPointerException.class;}
        else if(mode==5) {expected=new int[]{0,target};exact=true;}
        else if(mode==6) {expected=new int[]{0,target,3};exact=true;}
        else if(mode==7) {exact=true;}
        List<Integer> wanted=new ArrayList<Integer>();for(int event:expected)wanted.add(event);
        check(events.equals(wanted));
        if(exact)check(failure==FAILURE);
        else if(kind!=null)check(failure!=null&&failure.getClass()==kind);
        else {check(failure==null);if(route<2)check(raw(answer)==initial);}
        int field=mode<=5&&mode!=0?initial:mode==6?(route<2?initial:bits):LATE;
        check(raw(value.number)==field);
        Trace.hook=null;
    }
    public static void main(String[] args) {
        route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);attempt();attempt();
        System.out.println("attempts=2,primitive-parameter-bridge=matched");
    }
}
