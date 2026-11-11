import fixture.Core;
import fixture.Factory;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class RecursiveComparableExtremaHarness {
    static final List<Object> receivers=new ArrayList<>(),arguments=new ArrayList<>();
    static RuntimeException runtime;static Error error;static int mode;
    static final class Value implements Comparable<Object> {
        final int key;Value(int key){this.key=key;}
        public int compareTo(Object value) {
            receivers.add(this);arguments.add(value);
            if(mode==7)throw runtime;if(mode==8)throw error;
            return value==null?1:Integer.compare(key,((Value)value).key);
        }
    }
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned recursive comparable extrema mismatch: "+detail);
    }
    private static void attempt(int route,int selectedMode) {
        mode=selectedMode;runtime=new IllegalStateException("owned runtime");error=new LinkageError("owned error");receivers.clear();arguments.clear();
        Value low=new Value(1),high=new Value(3),middle=new Value(2),tie=new Value(3);Object plain=new Object();
        Collection input=new ArrayList();
        switch(mode) {
            case 0:input.add(low);input.add(high);input.add(middle);break;
            case 1:break;
            case 2:input=null;break;
            case 3:input.add(null);input.add(high);break;
            case 4:input.add(low);input.add(null);break;
            case 5:input.add(plain);break;
            case 6:input.add(low);input.add(plain);break;
            case 7:case 8:input.add(low);input.add(high);break;
            case 9:input.add(high);input.add(tie);break;
            default:throw new AssertionError("owned mode");
        }
        Factory.calls=0;Factory.last=null;Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(input);break;
                case 1:result=Core.checked(input);break;
                case 2:result=Core.relay(input);break;
                case 3:result=Core.sdk(input);break;
                default:throw new AssertionError("owned route");
            }
        }catch(Throwable caught){failure=caught;}
        check(Factory.calls==(route==3?0:1) && Factory.last==(route==3?null:input),"original provider and exact collection");
        int comparisons=mode==0?2:mode==3 || mode==7 || mode==8 || mode==9?1:0;
        check(receivers.size()==comparisons && arguments.size()==comparisons,"exact comparison count");
        if(comparisons>0) {
            check(receivers.get(0)==(mode==9?tie:high) && arguments.get(0)==(mode==3?null:mode==9?high:low),"first receiver and argument identity");
            if(comparisons==2)check(receivers.get(1)==middle && arguments.get(1)==high,"second comparison follows selected maximum");
        }
        if(mode==1)check(failure instanceof NoSuchElementException,"original empty failure");
        else if(mode==2 || mode==4)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==6 || mode==5 && route==1)check(failure instanceof ClassCastException,"original delayed Comparable cast");
        else if(mode==7)check(failure==runtime,"comparison runtime identity");
        else if(mode==8)check(failure==error,"comparison Error identity");
        else check(failure==null && result==(mode==5?plain:high),"original Object-erased result and maximum identity");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,recursive-comparable-extrema=matched");
    }
}
