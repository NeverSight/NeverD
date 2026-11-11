import fixture.*;
public class PackageOwnerConstructorHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned package owner constructor");}
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        int[] hints={Integer.MIN_VALUE,Integer.MAX_VALUE,0,-1,0x12345678,0x87654321};
        Policy start=new PackageOwnerConstructorProbe(),next=new PackageOwnerConstructorProbe.Child(),direct=mode==22?null:next;
        Object seed=mode==21?new Object():mode==22?null:start;Trace.seed=seed;Trace.next=next;boolean poisoned=kind==0&&mode>=3&&mode<=4||kind==0&&mode>=12&&mode<=13;
        Object first=mode%3==0?null:new Object(),second=mode%4==0?first:new Object();
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.hint=0;Trace.escaped=null;Trace.policy5=null;Trace.policy6=null;Trace.first5=null;Trace.first6=null;Trace.second5=null;Trace.second6=null;
            Trace.fail=mode>=3&&mode<12?mode-2:mode>=12&&mode<21?mode-11:0;
            Trace.failure=mode>=12&&mode<21?new AssertionError("owned error"):new IllegalStateException("owned runtime");
            Trace.mutate=kind==0&&mode==23;Trace.firstValue=first;Trace.secondValue=second;Trace.factoryPolicy=direct;
            int hint=hints[(mode+attempt)%hints.length];Policy expected=kind==0?(mode==22?null:mode==23&&attempt==1?next:start):direct;
            Core result=null;Throwable failure=null;try{result=PackageOwnerConstructorCaller.call(route,kind,hint,direct);}catch(Throwable e){failure=e;}
            int[] steps=kind==0?(attempt==0?new int[]{1,2,3,4,5,6,7,8}:new int[]{3,4,5,6,7,8}):kind==1?new int[]{9,3,4,5,6,7,8}:new int[]{3,4,5,6,7};
            long events=0;boolean escaped=false,assigned=false,firstDone=false,secondDone=false,observed5=false,observed6=false,done=false,enteredFactory=false;Throwable wanted=null;
            if(poisoned&&attempt==1){wanted=failure;require(failure instanceof NoClassDefFoundError);}
            else for(int step:steps) {
                if(step==3&&kind==0&&mode==21){wanted=failure;require(failure instanceof ClassCastException);break;}
                if(step==9)enteredFactory=true;if(step==3)escaped=true;if(step==4)assigned=true;if(step==5)observed5=true;if(step==6)observed6=true;
                events=events*10+step;
                if(step==Trace.fail) {
                    if(kind==0&&attempt==0&&(step==1||step==2)) {
                        wanted=failure;
                        if(Trace.failure instanceof Error)require(failure==Trace.failure);
                        else require(failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure);
                    } else wanted=Trace.failure;
                    break;
                }
                if(step==5)firstDone=true;if(step==6)secondDone=true;
                if(step==steps[steps.length-1])done=true;
            }
            require(failure==wanted&&Trace.events==events&&Trace.hint==(enteredFactory?hint:0));
            require((Trace.escaped!=null)==escaped&&((result!=null)==done));
            if(escaped)require(Trace.escaped.policy==(assigned?expected:null)&&Trace.escaped.first==(firstDone?first:null)&&Trace.escaped.second==(secondDone?second:null));
            if(done)require(result==Trace.escaped);
            require(Trace.policy5==(observed5?expected:null)&&Trace.policy6==(observed6?expected:null)&&Trace.first5==null&&Trace.first6==(observed6?first:null)&&Trace.second5==null&&Trace.second6==null);
            require(Trace.seed==seed&&Trace.firstValue==first&&Trace.secondValue==second&&Trace.factoryPolicy==direct);
            System.out.println("route="+route+",kind="+kind+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
        }
    }
}
