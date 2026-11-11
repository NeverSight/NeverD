package fixture;
import java.util.ArrayList;
import java.util.List;

public final class IntersectionFactoryResultHarness {
    static final RuntimeException FAILURE=new IllegalArgumentException("owned-failure");
    static final int LEFT=-19,RIGHT=Integer.MIN_VALUE;
    static int route,mode;
    static Client owner;
    static Factory result,late;
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean value) {
        if(!value)throw new AssertionError("owned intersection factory result mismatch");
    }
    static final class Result extends Factory {}
    static final class Observer extends Trace {
        public void observe(int phase,Object value,int left,int right,Object seen) {
            check(value==owner);
            check(left==(phase==6||phase==3?LEFT:0));
            check(right==(phase==7||phase==3?RIGHT:0));
            check(seen==(phase==3||phase==4&&route==0?result:null));
            events.add(phase);
            if(mode==3&&phase==1||mode==4&&phase==3||mode==5&&phase==4||mode==6&&phase==6||mode==7&&phase==7)throw FAILURE;
            if(phase==3)Factory.reply=late;
        }
    }
    static void attempt() {
        events.clear();owner=mode==2?null:new Client();
        if(owner!=null){owner.left=LEFT;owner.right=RIGHT;}
        result=mode==1?null:mode==8?new Result():mode==9?owner:new Client();late=new Result();Factory.reply=result;
        Trace.hook=new Observer();Object answer=null;Throwable failure=null;
        try {if(route==0)answer=owner.run();else owner.ignore();}catch(Throwable caught){failure=caught;}
        int[] expected={0,1,6,7,3,4};Class<?> kind=null;boolean exact=false;
        if(mode==2){expected=new int[]{};kind=NullPointerException.class;}
        else if(mode==3){expected=new int[]{0,1};exact=true;}
        else if(mode==4){expected=new int[]{0,1,6,7,3};exact=true;}
        else if(mode==5){exact=true;}
        else if(mode==6){expected=new int[]{0,1,6};exact=true;}
        else if(mode==7){expected=new int[]{0,1,6,7};exact=true;}
        else if(mode==8&&route==0){kind=ClassCastException.class;}
        List<Integer> wanted=new ArrayList<Integer>();for(int event:expected)wanted.add(event);
        check(events.equals(wanted));
        if(exact)check(failure==FAILURE);
        else if(kind!=null)check(failure!=null&&failure.getClass()==kind);
        else {check(failure==null&&answer==(route==0?result:null));}
        Trace.hook=null;
    }
    public static void main(String[] args) {
        route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);attempt();attempt();
        System.out.println("attempts=2,intersection-factory-result=matched");
    }
}
