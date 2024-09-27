function plot_thp_delay_new(a,id,Tlim,maxThp,maxDelay)
% This function plots the thorughput 
% RTT and estimated queue delay
% Parameters:
%  a        : log file from SCReAM BW test tool
%             imported with the command
%             a = load(<logfile>);
%             where <logfile> is the name of the log file      
%  id       * stream id
%  Tlim     : xmin and xmax limits [s], e.g. [0 100]
%  maxThp   : Max thorughput [Mbps]
%  maxDelay : Max delay [s]
%
% The script can be used with matlab or octave
% 
ix = find(a(:,8)==id);

T = a(ix,1);
T = T-T(1);
K = 10;
B = ones(1,K)/K;
A = [1 -0.95];


subplot(4,1,1);
%plot(T,filter(1,1,a(ix,13))/1e6,T,filter(B,1,a(ix,12))/1e6,T,filter(B,1,a(ix,14))/1e6)
plot(T,filter(1,1,a(ix,13))/1e6,T,filter(0.05,A,a(ix,12))/1e6,T,filter(0.05,A,a(ix,14))/1e6,T,a(ix,18)/1e6);
set(gca,'FontSize',12);grid on;
set(gca,'XTickLabel',[]);grid on;
title('Throughput [Mbps]');
legend('Transmitted','RTP','ACKed','Target')
xlim(Tlim);
ylim([0 maxThp]);

subplot(4,1,2);
plot(T,a(ix,5)/1e3,'-R',T,a(ix,4)/1e3,'.-B');
set(gca,'FontSize',12);grid on;
set(gca,'XTickLabel',[]);grid on;
title('CWND/B) and bytes in flight(R) [kByte]');
xlim(Tlim);


subplot(4,1,3);
K = 10;
B = ones(1,K)/K;
x = a(ix,11)./a(ix,10)*100;
plot(T,filter(B,1,x));
mean(x)
set(gca,'FontSize',12);grid on;
set(gca,'XTickLabel',[]);grid on;
title('Congestion mark [%]');
xlim(Tlim);
ylim([0 110]);


K = 10;
B = ones(1,K)/K;

subplot(4,1,4);
plot(T,a(ix,2),'.-',T,filter(B,1,a(ix,19)),'.-','linewidth',1);ylim([0 maxDelay]);
%plot(T,a(ix,2)+a(ix,19),'R.',T,a(ix,2),'B.-','linewidth',1);ylim([0 maxDelay]);
set(gca,'FontSize',12);grid on;
title('Network queue(B) and RTP Q (R) [s]');
xlim(Tlim);


xlabel('T [s]');
xlim(Tlim);
end

