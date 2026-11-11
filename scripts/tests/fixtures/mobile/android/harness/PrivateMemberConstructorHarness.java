package fixture;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

public final class PrivateMemberConstructorHarness {
    static final RuntimeException FAILURE=new IllegalArgumentException("owned-failure");
    static final Throwable PROBLEM=new Throwable("owned-problem");
    static final int CODE=-19;
    static int route,mode;
    static Runnable partial;
    static Collection<Host.Callback> input,originalData;
    static List<Host.Callback> copied;
    static Host.Callback callback,replacement;
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean value) {
        if(!value)throw new AssertionError("owned private member constructor mismatch");
    }
    static final class Reply extends Host.Callback {
        final boolean original;
        Reply(boolean value) {original=value;}
        public void fire(int code) {
            check(original&&code==CODE);events.add(6);
            if(mode==7)throw FAILURE;
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase,Runnable value,Collection<Host.Callback> data,int code,Throwable problem) {
            check(code==CODE);
            if(phase==1) {partial=value;originalData=data;}
            check(value==partial);
            check(problem==(route==2?PROBLEM:null));
            if(phase==1) {
                check(Host.values(value)==null&&Host.code(value)==0&&Host.problem(value)==null);
                if(route!=0)check(data==input);
            } else {
                check(Host.code(value)==CODE&&Host.problem(value)==(route==2?PROBLEM:null));
                if(copied==null)copied=Host.values(value);
                check(copied==Host.values(value)&&copied!=originalData);
                check(copied.size()==1&&copied.get(0)==callback);
                check(data==(phase==4?null:phase==5?copied:originalData));
            }
            check(Host.original(value)==(route==0&&phase>=4?callback:null));
            events.add(phase);
            if(mode==2&&phase==1||mode==3&&phase==2||mode==4&&phase==3||mode==5&&phase==4||mode==6&&phase==5)throw FAILURE;
            if(mode==1&&phase==2)((List<Host.Callback>)data).set(0,replacement);
        }
    }
    static void attempt() {
        events.clear();partial=null;copied=null;originalData=null;
        callback=mode==8?null:new Reply(true);replacement=new Reply(false);
        List<Host.Callback> data=new ArrayList<Host.Callback>();data.add(callback);
        input=mode==9&&route!=0?null:data;
        Trace.hook=new Observer();Runnable made=null;Throwable failure=null;
        try {
            made=route==0?Host.single(callback,CODE):route==1?Host.group(input,CODE):Host.explicit(input,CODE,PROBLEM);
            made.run();
        } catch(Throwable caught) {failure=caught;}
        int[] expected=route==0?new int[]{1,2,4,5,6}:route==1?new int[]{1,2,3,5,6}:new int[]{1,2,5,6};
        boolean exact=false;Class<?> kind=null;
        if(mode==8&&route==0) {expected=new int[]{};kind=NullPointerException.class;}
        else if(mode==2) {expected=new int[]{1};exact=true;}
        else if(mode==9&&route!=0) {expected=new int[]{1};kind=NullPointerException.class;}
        else if(mode==3) {expected=new int[]{1,2};exact=true;}
        else if(mode==4&&route==1) {expected=new int[]{1,2,3};exact=true;}
        else if(mode==5&&route==0) {expected=new int[]{1,2,4};exact=true;}
        else if(mode==6) {expected=route==0?new int[]{1,2,4,5}:route==1?new int[]{1,2,3,5}:new int[]{1,2,5};exact=true;}
        else if(mode==7) {exact=true;}
        else if(mode==8) {expected=route==1?new int[]{1,2,3,5}:new int[]{1,2,5};kind=NullPointerException.class;}
        List<Integer> wanted=new ArrayList<Integer>();for(int event:expected)wanted.add(event);
        check(events.equals(wanted));
        if(exact)check(failure==FAILURE);
        else if(kind!=null)check(failure!=null&&failure.getClass()==kind);
        else check(failure==null&&made==partial);
        Trace.hook=null;
    }
    public static void main(String[] args) {
        route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);attempt();attempt();
        System.out.println("attempts=2,private-member-constructor=matched");
    }
}
